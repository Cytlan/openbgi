/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sprite.c - Sprite, the general-purpose image object (class order 2).
 * Interface in inc/bgi/gfx/sprite.h.
 *
 * A sprite is in one of six modes, each selected by one "set" instruction:
 * "90 56" plain, "90 58" blend, "90 59" transform, "90 5A" wipe, "90 5B"
 * ripple, "90 5C" projected.  The file follows that order of use: the "set"
 * entry points, which also place the sprite; the mode cores, which validate
 * the bitmaps and fill in the mode state; the vtable overrides (the level
 * routing of the blend modes, the sprite's parameters of "90 38" / "91 38");
 * the caches (cross-fade, skew, ring table, wipe pattern); the geometry of
 * the transformed modes (bounding box, animation, projection); and
 * Sprite_Draw, which paints the dirty piece in every mode - through a
 * temporary bitmap whenever a mask or an effect the mode's blitter cannot
 * apply itself is in play.
 *
 * The bounding-box arithmetic of modes 2 and 5 (Sprite_BoundsProjected) is
 * written in doubles in the same operation order as the original, with its
 * truncation rules (Floor16 / Ceil16), so that the boxes match it pixel for
 * pixel.
 */
#include <math.h>
#include <string.h>

#include "bgi/common.h"
#include "bgi/gfx/sprite.h"
#include "bgi/gfx/gfxmgr.h" // the projection centre of 1.535 on
#include "bgi/version.h"    // the parameters the later builds added

/* the projection distance of the perspective scale: 0 (no depth scaling)
 * while setParam 0x8001 of 1.494 on is set */
#define SpProjDist(s) ((s)->obj.noPerspective ? 0 : (s)->projDist)

#define DEG16_TO_RAD  2.663161090079238e-07 // 2*pi / (360 << 16): 16.16 degrees to radians
#define INV_65536     1.52587890625e-05     // 1 / 65536: 16.16 to double

// ========================================================================
// helpers
// ========================================================================

// the descriptor of bitmap `no`; 0 when there is no such bitmap
static int SpGetInfo(Bmp_t* out, int no)
{
	return BmpMgr_GetInfo(gDispBmpMgr, out, no);
}

// the generation counter of bitmap slot `no` (bumped on every load / free)
static int SpGen(int no)
{
	return BmpMgr_Generation(gDispBmpMgr, no);
}

// the descriptor of bitmap `no`, but only while it still is the bitmap of generation `gen` the sprite was set with
static int SpCurrent(Bmp_t* out, int no, int gen)
{
	return SpGetInfo(out, no) && SpGen(no) == gen;
}

// (a * b) >> sh in 64 bits with a sign-extended and b zero-extended
static int32_t MulShr(int32_t a, uint32_t b, int sh)
{
	uint64_t p = (uint64_t)(int64_t)a * (uint64_t)b;
	return (int32_t)((int64_t)p >> sh);
}

// the effects the transform, cross-fade and wipe blitters apply themselves (0 alpha copy, 1 and 0x20 blend); any other goes through a temporary and Bmp_BlitEffect
static int DirectEffect(int e)
{
	return e == 0 || e == 1 || e == 0x20;
}

// the four "drop the caches of the previous mode" calls every setter makes
static void Sprite_DropCaches(Sprite_t* s)
{
	Sprite_FreeMix(s);
	Sprite_FreeSkew(s);
	Sprite_FreeRingTable(s);
	Sprite_FreeWipeGray(s);
}

// free the pixels of a private bitmap and clear its descriptor; *had tells whether there were any
static void FreeBmpSlot(Bmp_t* b, int* had)
{
	*had = b->pixels != NULL;
	if(*had)
	{
		BGI_Free(b->pixels);
		memset(b, 0, sizeof *b);
	}
}

// ========================================================================
// construction
// ========================================================================

// invisible, sizeless, mode 0 with no bitmap
void Sprite_Ctor(Sprite_t* s, int slotId)
{
	DispObj_Ctor(&s->obj, 2, slotId);
	s->obj.vt = &Sprite_Vtbl;
	s->bmp1 = -1;
	s->levelRoute = -1;
	s->mode = 0;
	s->maskOn = 0;
	memset(&s->mixCache, 0, sizeof s->mixCache);
	s->ringTable = NULL;
	memset(&s->wipeGray, 0, sizeof s->wipeGray);
	s->skewOn = 0;
	memset(&s->skewCache, 0, sizeof s->skewCache);
	s->zoomX = s->zoomY = 0x10000;
	s->maskObj = s->maskUser = NULL;
}

// unlink the mask sprite in both directions, drop the caches, then the base destructor
void Sprite_Dtor(Sprite_t* s)
{
	s->obj.vt = &Sprite_Vtbl;
	Sprite_LinkMask(s, NULL); // (the links of "91 55" cannot outlive either side)
	if(s->maskUser)
		Sprite_LinkMask(s->maskUser, NULL);
	Sprite_DropCaches(s);
	DispObj_Dtor(&s->obj);
}

/* "91 55" of 1.529 on: mask this sprite by the picture of another one,
 * placed where that sprite is; NULL removes the link.  The bitmap mask of
 * "90 55" is dropped first.  A mask sprite serves one sprite at a time.
 * 0 ok, 0x8000000B already linked, 0x8000000C nothing to unlink,
 * 0x8000000D the mask sprite is in use, 0x8000000E the mask sprite had no
 * user. */
uint32_t Sprite_LinkMask(Sprite_t* s, Sprite_t* mask)
{
	if(mask)
	{
		Sprite_SetMask(s, -1);
		if(s->maskObj)
			return 0x8000000bu;
		if(mask->maskUser)
			return 0x8000000du;
		mask->maskUser = s;
		s->maskObj = mask;
		return 0;
	}
	if(!s->maskObj)
		return 0x8000000cu;
	if(s->maskObj->maskUser != s)
		return 0x8000000eu;
	s->maskObj->maskUser = NULL;
	s->maskObj = NULL;
	return 0;
}

static void Sprite_Destroy(DispObj_t* o, int flags)
{
	Sprite_Dtor((Sprite_t*)o);
	if(flags & 1)
		BGI_Free(o);
}

// ========================================================================
// the "set" instructions
// ========================================================================

// "90 56": place the sprite with one bitmap, effect, level and priority; the Sprite_SetPlain result
int Sprite_Set(Sprite_t* s, int x, int y, int bmp, int effect, int level, int prio)
{
	int r = Sprite_SetPlain(s, bmp);
	if(r == 0)
	{
		s->obj.vt->setPos(&s->obj, x, y);
		DispObj_SetEffect(&s->obj, effect);
		s->obj.vt->setLevel(&s->obj, level);
		DispObj_SetPriority(&s->obj, (uint32_t)prio);
	}
	return r;
}

/* "90 58": the blend mode placed; the effect is forced to 1 (blend) and the
 * level is stored through the base class (the override would route it by
 * levelRoute).  The Sprite_SetBlend result. */
int Sprite_SetBlendEx(Sprite_t* s, int x, int y, int bmp1, int bmp2, int mixRatio, int level, int prio,
	int levelRoute)
{
	int r = Sprite_SetBlend(s, bmp1, bmp2, mixRatio, levelRoute);
	if(r == 0)
	{
		s->obj.vt->setPos(&s->obj, x, y);
		DispObj_SetEffect(&s->obj, 1);
		DispObj_SetLevel(&s->obj, level);
		DispObj_SetPriority(&s->obj, (uint32_t)prio);
	}
	return r;
}

/* "90 59": the transform mode placed.  The animation deltas and the curve
 * are cleared and the common state is set first, then the transform; the
 * Sprite_SetTransform result (the common state stays set when it fails). */
int Sprite_SetTransformEx(Sprite_t* s, int x, int y, int bmp, int ox, int oy, int32_t angle, int32_t sx,
	int32_t sy, int smooth, int effect, int level, int prio)
{
	s->dOx = s->dOy = s->dAngle = s->dSx = s->dSy = s->curve = 0;
	s->obj.vt->setPos(&s->obj, x, y);
	DispObj_SetEffect(&s->obj, effect);
	s->obj.vt->setLevel(&s->obj, level);
	DispObj_SetPriority(&s->obj, (uint32_t)prio);
	return Sprite_SetTransform(s, bmp, ox, oy, angle, sx, sy, smooth);
}

// "90 5A": the wipe mode placed, with the blitter's mode and the starting progress; the Sprite_SetWipe result
int Sprite_SetWipeEx(Sprite_t* s, int x, int y, int bmp, int gray, int wipeMode, int progress, int effect,
	int level, int prio)
{
	int r = Sprite_SetWipe(s, bmp, gray);
	if(r == 0)
	{
		s->obj.vt->setPos(&s->obj, x, y);
		s->wipeMode = wipeMode;
		DispObj_SetProgressInt(&s->obj, progress);
		DispObj_SetEffect(&s->obj, effect);
		s->obj.vt->setLevel(&s->obj, level);
		DispObj_SetPriority(&s->obj, (uint32_t)prio);
	}
	return r;
}

/* "90 5B": the ripple mode placed.  `level` is the ripple amplitude
 * (consumed by Sprite_SetRipple), `fade` the transparency the blitter
 * draws with; the effect is forced to 1.  The Sprite_SetRipple result. */
int Sprite_SetRippleEx(Sprite_t* s, int x, int y, int bmp, int map, int rings, int rippleNo, int level,
	int fade, int prio)
{
	int r = Sprite_SetRipple(s, bmp, map, rings, rippleNo, level);
	if(r == 0)
	{
		s->obj.vt->setPos(&s->obj, x, y);
		DispObj_SetEffect(&s->obj, 1);
		DispObj_SetFade(&s->obj, fade);
		DispObj_SetPriority(&s->obj, (uint32_t)prio);
	}
	return r;
}

/* "90 5C": the projected mode placed: depth ordering on, the position from
 * the 16.16 triple; the deltas and the curve are cleared.  The
 * Sprite_SetProjected result. */
int Sprite_SetProjectedEx(Sprite_t* s, int32_t fx, int32_t fy, int32_t fz, int bmp1, int bmp2, int mixRatio,
	int levelRoute, int ox, int oy, int32_t angle, int projDist, int projPos,
	int smooth, int effect, int level, int prio)
{
	s->dOx = s->dOy = s->dAngle = s->dSx = s->dSy = s->curve = 0;
	DispObj_SetFlat(&s->obj, 0);
	s->obj.vt->setFixedPos(&s->obj, fx, fy, fz);
	DispObj_SetEffect(&s->obj, effect);
	s->obj.vt->setLevel(&s->obj, level);
	DispObj_SetPriority(&s->obj, (uint32_t)prio);
	return Sprite_SetProjected(s, bmp1, bmp2, mixRatio, levelRoute, ox, oy, angle, projDist, projPos, smooth);
}

/* "90 57" (and setParam 0x10 of 1.573 on): replace the bitmap, keeping the
 * mode's parameters.  Modes 0, 2 and 5 only (mode 5 loses its second
 * bitmap); the other modes return 0 without doing anything.  Else the
 * mode core's result. */
int Sprite_ChangeBitmap(Sprite_t* s, int bmp)
{
	switch(s->mode)
	{
		case 0:
			return Sprite_SetPlain(s, bmp);
		case 2:
			return Sprite_SetTransform(s, bmp, (int16_t)(s->ox >> 16), (int16_t)(s->oy >> 16),
				s->angle, s->sx, s->sy, s->smooth);
		case 5:
			return Sprite_SetProjected(s, bmp, -1, 0, -1, (int16_t)(s->ox >> 16), (int16_t)(s->oy >> 16),
				s->angle, s->projDist, s->projPos, s->smooth);
		default:
			return 0;
	}
}

// ========================================================================
// mode cores
// ========================================================================

// mode 0: one bitmap; 0 ok, 0x80000001 no such bitmap
int Sprite_SetPlain(Sprite_t* s, int bmp)
{
	Bmp_t b;
	if(!SpGetInfo(&b, bmp))
		return (int)0x80000001;
	Sprite_DropCaches(s);
	s->mode = 0;
	s->bmp1 = bmp;
	s->gen1 = SpGen(bmp);
	s->levelRoute = -1;
	s->obj.vt->setSize(&s->obj, b.w, b.h);
	return 0;
}

/* mode 1: two bitmaps of the same size cross-faded by mixRatio (0 = the
 * first, 0x100 = the second); `levelRoute` says what setLevel drives from
 * now on (-1 / 0 the level, 1 the mix ratio, 2 both).  0 ok, 0x80000001 /
 * 0x80000002 no such bitmap, 0x80000003 the sizes differ. */
int Sprite_SetBlend(Sprite_t* s, int bmp1, int bmp2, int mixRatio, int levelRoute)
{
	Bmp_t a, b;
	if(!SpGetInfo(&a, bmp1))
		return (int)0x80000001;
	if(!SpGetInfo(&b, bmp2))
		return (int)0x80000002;
	if(a.w != b.w || a.h != b.h)
		return (int)0x80000003;
	Sprite_DropCaches(s);
	s->mode = 1;
	s->bmp1 = bmp1;
	s->bmp2 = bmp2;
	s->gen1 = SpGen(bmp1);
	s->gen2 = SpGen(bmp2);
	s->mixRatio = mixRatio;
	s->levelRoute = levelRoute;
	s->obj.vt->setSize(&s->obj, a.w, a.h);
	return 0;
}

/* mode 2: the bitmap rotated by `angle` (16.16 degrees) and scaled by
 * (sx, sy) (16.16) about the origin (ox, oy), in pixels inside the bitmap;
 * `smooth` selects bilinear sampling.  The object's size becomes the
 * bounding box of the transformed picture.  0 ok, 0x80000001 no such
 * bitmap, 0x80000004 the transformed picture is smaller than 2 x 2. */
int Sprite_SetTransform(Sprite_t* s, int bmp, int ox, int oy, int32_t angle, int32_t sx, int32_t sy, int smooth)
{
	Bmp_t b;
	int32_t centre[2], cAngle, cSx, cSy, outW, outH, anchor[2];
	int32_t ox16 = (int32_t)((uint32_t)ox << 16), oy16 = (int32_t)((uint32_t)oy << 16);
	if(!SpGetInfo(&b, bmp))
		return (int)0x80000001;
	Sprite_Animate(s, centre, &cAngle, &cSx, &cSy, ox16, oy16, angle, sx, sy);
	Sprite_BoundsTransform(&outW, &outH, anchor, b.w, b.h, 0, centre[0], centre[1], cAngle, cSx, cSy);
	if((uint32_t)outW < 2 || (uint32_t)outH < 2)
		return (int)0x80000004;
	Sprite_DropCaches(s);
	s->mode = 2;
	s->zoomX = s->zoomY = 0x10000;
	s->bmp1 = bmp;
	s->gen1 = SpGen(bmp);
	s->sy = sy;
	s->sx = sx;
	s->angle = angle;
	s->curOx = centre[0];
	s->curOy = centre[1];
	s->curSy = cSy;
	s->curSx = cSx;
	s->curAngle = cAngle;
	s->smooth = smooth;
	s->anchorX = anchor[0];
	s->anchorY = anchor[1];
	s->outH = outH;
	s->outW = outW;
	s->levelRoute = -1;
	s->ox = ox16;
	s->oy = oy16;
	s->obj.vt->setSize(&s->obj, outW, outH);
	return 0;
}

/* mode 3: a bitmap revealed through a grayscale pattern as the progress
 * advances; the pattern is copied into a private gray bitmap of the
 * picture's size (a smaller pattern leaves the rest at 0).  0 ok,
 * 0x80000001 no such bitmap, 0x80000002 no such pattern, 0x8000000A the
 * pattern is not GRAY8. */
int Sprite_SetWipe(Sprite_t* s, int bmp, int gray)
{
	Bmp_t a, g;
	if(!SpGetInfo(&a, bmp))
		return (int)0x80000001;
	if(!SpGetInfo(&g, gray))
		return (int)0x80000002;
	if(g.mode != PM_GRAY8)
		return (int)0x8000000a;
	Sprite_DropCaches(s);
	Bmp_Alloc(&s->wipeGray, a.w, a.h, PM_GRAY8);
	Bmp_Clear(&s->wipeGray, NULL);
	Bmp_Blit(&s->wipeGray, 0, 0, &g, 0x80, 0);
	s->mode = 3;
	s->bmp1 = bmp;
	s->gen1 = SpGen(bmp);
	s->levelRoute = -1;
	s->obj.vt->setSize(&s->obj, a.w, a.h);
	return 0;
}

/* mode 4: a bitmap distorted through a vector + distance map (pixel mode 6)
 * of the same size, driven by ripple definition `rippleNo` ("92 00") with
 * `rings` rings; `level` is the amplitude, applied by setLevel.  0 ok,
 * 0x80000001 no such bitmap, 0x8000000A the bitmap is empty, 0x80000005 no
 * such map, 0x80000006 the map is not mode 6 or differs in size,
 * 0x80000007 no rings, 0x80000008 no such ripple definition, 0x80000009
 * the definition has fewer rings than asked for. */
int Sprite_SetRipple(Sprite_t* s, int bmp, int map, int rings, int rippleNo, int level)
{
	Bmp_t a, m;
	uint32_t* table;
	int ok = 0;
	if(!SpGetInfo(&a, bmp))
		return (int)0x80000001;
	if((uint32_t)a.w == 0 || (uint32_t)a.h == 0)
		return (int)0x8000000a;
	if(!SpGetInfo(&m, map))
		return (int)0x80000005;
	if(m.mode != 6 || m.w != a.w || m.h != a.h)
		return (int)0x80000006;
	if((uint32_t)rings == 0)
		return (int)0x80000007;
	table = (uint32_t*)BGI_Alloc((size_t)rings * 16);
	if(BmpMgr_RippleCheck(gDispBmpMgr, &ok, rippleNo, 0, rings) != 0)
	{
		BGI_Free(table);
		return (int)0x80000008;
	}
	if(!ok)
	{
		BGI_Free(table); // the original leaks it
		return (int)0x80000009;
	}
	Sprite_DropCaches(s);
	s->mode = 4;
	s->levelRoute = -1;
	s->bmp1 = bmp;
	s->rippleMap = map;
	s->gen1 = SpGen(bmp);
	s->rippleMapGen = SpGen(map);
	s->rings = rings;
	s->ringTable = table;
	s->rippleNo = rippleNo;
	s->rippleSel = 0;
	s->obj.vt->setLevel(&s->obj, level);
	s->obj.vt->setSize(&s->obj, a.w, a.h);
	return 0;
}

/* mode 5: mode 2 in 3-D.  Both scale factors are the perspective factor of
 * the depth at `projDist` (`projPos` also scales the position by it); a
 * second bitmap of the same size and mode is cross-faded into a cache.
 * The object switches to depth ordering (the caller does that).  0 ok,
 * 0x80000001 / 0x80000002 no such bitmap, 0x80000003 the two bitmaps differ
 * in size or mode, 0x80000004 the projected picture is smaller than 2 x 2. */
int Sprite_SetProjected(Sprite_t* s, int bmp1, int bmp2, int mixRatio, int levelRoute, int ox, int oy,
	int32_t angle, int projDist, int projPos, int smooth)
{
	Bmp_t a, b;
	int hasB = bmp2 != -1;
	int32_t fp[4], scale, centre[2], cAngle, cSx, cSy, outW, outH, anchor[2], skew;
	int32_t ox16 = (int32_t)((uint32_t)ox << 16), oy16 = (int32_t)((uint32_t)oy << 16);
	if(!SpGetInfo(&a, bmp1))
		return (int)0x80000001;
	if(hasB)
	{
		if(!SpGetInfo(&b, bmp2))
			return (int)0x80000002;
		if(a.w != b.w || a.h != b.h || a.mode != b.mode)
			return (int)0x80000003;
	}
	DispObj_EffFixedPos(&s->obj, fp); // with the "91 37" / "92 37" offsets of 1.69/472 on
	ProjectScale(&scale, fp[2], s->obj.noPerspective ? 0 : projDist);
	Sprite_Animate(s, centre, &cAngle, &cSx, &cSy, ox16, oy16, angle, scale, scale);
	skew = (uint32_t)s->skewOn > 0 ? s->skewAmount : 0;
	Sprite_BoundsProjected(&outW, &outH, anchor, a.w, a.h, skew, centre[0], centre[1], cAngle, cSx, cSy,
		fp[0], fp[1]);
	if((uint32_t)outW < 2 || (uint32_t)outH < 2)
		return (int)0x80000004;
	Sprite_DropCaches(s);
	s->mode = 5;
	s->zoomX = s->zoomY = 0x10000;
	s->bmp1 = bmp1;
	s->bmp2 = bmp2;
	s->gen1 = SpGen(bmp1);
	s->gen2 = hasB ? SpGen(bmp2) : 0;
	s->mixRatio = hasB ? mixRatio : 0;
	s->levelRoute = hasB ? levelRoute : -1;
	s->angle = angle;
	s->sx = s->sy = scale;
	s->smooth = smooth;
	s->projDist = projDist;
	s->projPos = projPos;
	s->curAngle = cAngle;
	s->curOx = centre[0];
	s->curOy = centre[1];
	s->outW = outW;
	s->curSx = cSx;
	s->curSy = cSy;
	s->anchorY = anchor[1];
	s->outH = outH;
	s->anchorX = anchor[0];
	s->ox = ox16;
	s->oy = oy16;
	Sprite_RebuildMix(s);
	Sprite_RebuildSkew(s);
	Sprite_ProjectPosition(s);
	s->obj.vt->setSize(&s->obj, outW, outH);
	return 0;
}

/* "90 55": the mask bitmap, addressed in screen coordinates (the sprite
 * shows where the mask is set), -1 for none.  A screen-sized GRAY8 bitmap
 * up to 1.494; from 1.529 on a GRAY8 bitmap of any size, screen aligned
 * (the sprite shows only where the two overlap), and from 1.599 on an
 * ARGB32 one as well, whose alpha is the mask.  0 ok, 0x80000001 no such
 * bitmap, 0x8000000A not a usable mask. */
int Sprite_SetMask(Sprite_t* s, int bmp)
{
	Bmp_t info, back;
	int ok;
	if(s->maskObj && bmp != -1) // 1.529 on: a bitmap mask replaces the linked sprite
		Sprite_LinkMask(s, NULL);
	if(bmp == -1)
	{
		s->maskBmp = -1;
		s->maskOn = 0;
		return 0;
	}
	if(!SpGetInfo(&info, bmp))
		return (int)0x80000001;
	Gfx_CopyBackBmp(gDispGfx, &back);
	if(gEngine->gen >= GEN_1_599)
		ok = info.mode == PM_GRAY8 || info.mode == PM_ARGB32;
	else if(gEngine->gen >= GEN_1_529)
		ok = info.mode == PM_GRAY8;
	else
		ok = info.mode == PM_GRAY8 && info.w == back.w && info.h == back.h;
	if(!ok)
		return (int)0x8000000a;
	s->maskOn = 1;
	s->maskBmp = bmp;
	s->maskGen = SpGen(bmp);
	return 0;
}

// ========================================================================
// small overrides and setters
// ========================================================================

// setParam 0x40: the origin of modes 2 and 5, in pixels (not recomputed until the next animation step)
void Sprite_SetOrigin(Sprite_t* s, int x, int y)
{
	if(s->mode == 2 || s->mode == 5)
	{
		s->ox = (int32_t)((uint32_t)x << 16);
		s->oy = (int32_t)((uint32_t)y << 16);
	}
}

// setParam 0x41: the angle of modes 2 and 5 (16.16 degrees); the projected recompute is used for mode 2 as well
void Sprite_SetAngle(Sprite_t* s, int32_t angle)
{
	if(s->mode == 2 || s->mode == 5)
	{
		s->angle = angle;
		Sprite_ProjectPosition(s);
		Sprite_RecalcProjected(s);
	}
}

// in modes 2 and 5 the position is where the origin lands, so the box starts `anchor` before it
static void Sprite_GetDrawPos(DispObj_t* o, int32_t out[2])
{
	Sprite_t* s = (Sprite_t*)o;
	DispObj_GetDrawPos(o, out);
	if(s->mode == 2 || s->mode == 5)
	{
		out[0] -= s->anchorX;
		out[1] -= s->anchorY;
	}
}

// "91 33": a new depth changes the projected scale and box
static void Sprite_SetFixedPos(DispObj_t* o, int32_t fx, int32_t fy, int32_t fz)
{
	Sprite_t* s = (Sprite_t*)o;
	DispObj_SetFixedPos(o, fx, fy, fz);
	if(s->mode == 5)
	{
		Sprite_ProjectPosition(s);
		Sprite_RecalcProjected(s);
	}
}

/* "90 32" and the fades: one tween can drive the transparency, the
 * cross-fade or both, as levelRoute says (-1 / 0 the level, 1 the mix
 * ratio, 2 both).  In ripple mode the level is the amplitude and refills
 * the ring table. */
static void Sprite_SetLevel(DispObj_t* o, int level)
{
	Sprite_t* s = (Sprite_t*)o;
	switch(s->levelRoute)
	{
		case 2:
			DispObj_SetLevel(o, level);
			s->mixRatio = level;
			Sprite_RebuildMix(s);
			Sprite_RebuildSkew(s);
			break;
		case 1:
			s->mixRatio = level;
			Sprite_RebuildMix(s);
			Sprite_RebuildSkew(s);
			break;
		case 0:
			DispObj_SetLevel(o, level);
			break;
		case -1:
			if(s->mode == 4)
				BmpMgr_RippleFill(gDispBmpMgr, s->ringTable, s->rippleNo, s->rippleSel, level, s->rings);
			DispObj_SetLevel(o, level);
			break;
		default:
			break;
	}
}

// the value Sprite_SetLevel drives: the mix ratio for route 1, else the level
static int Sprite_GetLevel(DispObj_t* o)
{
	Sprite_t* s = (Sprite_t*)o;
	switch(s->levelRoute)
	{
		case 1: return s->mixRatio;
		case 0:
		case 2:
		case -1: return DispObj_GetLevel(o);
		default: return 0; // uninitialised in the original
	}
}

// "90 35": a new progress moves the animated transform of modes 2 and 5
static void Sprite_SetProgress(DispObj_t* o, int raw, int v)
{
	Sprite_t* s = (Sprite_t*)o;
	DispObj_SetProgress(o, raw, v);
	if(s->mode == 2)
		Sprite_RecalcTransform(s);
	else if(s->mode == 5)
		Sprite_RecalcProjected(s);
}

/* setParam 0x60: the sine-wave skew of mode 5.  `on` doubles as the
 * period of the wave in rows (0 turns it off), `arg` is its phase in rows,
 * `amount` (16.16) how far a row is shifted, see Blit_Skew.  Stored for
 * every mode, applied in mode 5 only. */
void Sprite_SetSkew(Sprite_t* s, int on, int arg, int32_t amount)
{
	s->skewOn = on;
	s->skewArg = arg;
	s->skewAmount = amount;
	if(s->mode == 5)
	{
		Sprite_RebuildSkew(s);
		Sprite_RecalcProjected(s);
	}
}

/* "91 38" (1.69/472 on): the sprite's own readable parameters - 0x41 the
 * angle (modes 2 and 5), 0x10000000 the animated transform of mode 5
 * (centre x, y, angle, scale x, y: five values), 0x10000100 the bitmap's
 * size and the transformed size (four values; the bitmap's size is left
 * untouched when the bitmap is gone).  Anything else goes to the base
 * class.  0 ok, 0xffff0001 wrong mode. */
static int Sprite_GetParam(DispObj_t* o, int no, int32_t* out)
{
	Sprite_t* s = (Sprite_t*)o;
	Bmp_t a;
	switch(no)
	{
		case 0x41:
			if(s->mode != 2 && s->mode != 5)
				return (int)0xffff0001;
			out[0] = s->angle;
			return 0;
		case 0x10000000:
		{
			int32_t centre[2], angle, sx, sy;
			if(s->mode != 5)
				return (int)0xffff0001;
			Sprite_Animate(s, centre, &angle, &sx, &sy, s->ox, s->oy, s->angle, s->sx, s->sy);
			out[0] = centre[0];
			out[1] = centre[1];
			out[2] = angle;
			out[3] = sx;
			out[4] = sy;
			return 0;
		}
		case 0x10000100:
			if(SpGetInfo(&a, s->bmp1))
			{
				out[0] = a.w;
				out[1] = a.h;
			}
			if(s->mode == 2 || s->mode == 5)
			{
				out[2] = s->outW;
				out[3] = s->outH;
			}
			else
			{
				out[2] = out[0];
				out[3] = out[1];
			}
			return 0;
		default:
			return DispObj_GetParam(o, no, out);
	}
}

/* "90 38": the sprite's parameters - 0x40 the origin (a, c), 0x41 the
 * angle, 0x42 (1.494 on) the zoom (one value until 1.573, a pair from
 * 1.588), 0x10 (1.573 on) the bitmap with the mode kept, 0x60 the skew
 * (a >> 16 the period, a & 0xFFFF the phase, c the amount), 0x80 the
 * origin delta (a, c), 0x81 the angle delta, 0x82 the scale delta (a, c),
 * 0x8F the easing curve of the angle delta, 0x100 the ripple phase (a the
 * selector, c the level).  The origin, angle and deltas are ignored outside
 * modes 2 and 5.  Anything else goes to the base class.  0 ok, 0xffff0002
 * when the bitmap change or the ripple selection failed. */
static int Sprite_SetParam(DispObj_t* o, int no, int a, int c)
{
	Sprite_t* s = (Sprite_t*)o;
	int anim = s->mode == 2 || s->mode == 5;
	switch(no)
	{
		case 0x40:
			if(anim)
				Sprite_SetOrigin(s, a, c);
			return 0;
		case 0x41:
			if(anim)
				Sprite_SetAngle(s, a);
			return 0;
		case 0x42: // 1.494 on; one value until 1.573
			if(gEngine->gen < GEN_1_494)
				break;
			Sprite_SetZoom(s, a, gEngine->gen >= GEN_1_588 ? c : 0);
			return 0;
		case 0x10: // 1.573 on: the bitmap, the mode kept
			if(gEngine->gen < GEN_1_573)
				break;
			return Sprite_ChangeBitmap(s, a) != 0 ? (int)0xffff0002 : 0;
		case 0x60:
			Sprite_SetSkew(s, (int)((uint32_t)a >> 16), a & 0xffff, c);
			return 0;
		case 0x80:
			if(anim)
			{
				s->dOx = a;
				s->dOy = c;
			}
			return 0;
		case 0x81:
			if(anim)
				s->dAngle = a;
			return 0;
		case 0x82:
			if(anim)
			{
				s->dSx = a;
				s->dSy = c;
			}
			return 0;
		case 0x8f:
			s->curve = a;
			return 0;
		case 0x100:
			return Sprite_SelectRipple(s, a, c) != 0 ? (int)0xffff0002 : 0;
		default:
			break;
	}
	return DispObj_SetParam(o, no, a, c);
}

/* "90 53": dirty the part of the sprite that shows the rectangle `r` of its
 * bitmap after the script drew into it; modes without a mapping dirty the
 * whole sprite.  1 when something was dirtied, 0 when the mapped rectangle
 * is empty. */
int Sprite_UpdateRect(Sprite_t* s, const Rect_t* r)
{
	Rect_t m;
	if(!Sprite_MapUpdateRect(s, &m, r))
	{
		s->obj.vt->invalidate(&s->obj);
		return 1;
	}
	if(m.l > m.r || m.t > m.b)
		return 0;
	Gfx_AddDirty(gDispGfx, s->obj.vt->sortKey(&s->obj), &m);
	return 1;
}

/* setParam 0x100: move the ripple of mode 4 to phase `selector` and set
 * its amplitude; a level above 0x100 keeps the current one.  0 ok (also
 * outside mode 4, where nothing happens), 0x80000008 no such ripple
 * definition, 0x80000009 not enough rings left from that phase. */
int Sprite_SelectRipple(Sprite_t* s, int selector, int level)
{
	int ok = 0;
	if(s->mode != 4)
		return 0;
	if(BmpMgr_RippleCheck(gDispBmpMgr, &ok, s->rippleNo, selector, s->rings) != 0)
		return (int)0x80000008;
	if(!ok)
		return (int)0x80000009;
	s->rippleSel = selector;
	if((uint32_t)level > 0x100)
		level = s->obj.vt->getLevel(&s->obj);
	s->obj.vt->setLevel(&s->obj, level);
	return 0;
}

// mode 5 with two bitmaps: mixCache = crossfade(bmp1, bmp2); dropped and left empty when either bitmap is gone
void Sprite_RebuildMix(Sprite_t* s)
{
	Bmp_t a, b;
	if(s->mode != 5)
		return;
	Sprite_FreeMix(s);
	if(s->bmp2 == -1)
		return;
	if(!SpGetInfo(&a, s->bmp1) || !SpGetInfo(&b, s->bmp2))
		return;
	if(SpGen(s->bmp1) != s->gen1 || SpGen(s->bmp2) != s->gen2)
		return;
	Bmp_Alloc(&s->mixCache, a.w, a.h, a.mode);
	Blit_CrossfadeTo(&s->mixCache, &a, &b, s->mixRatio);
}

// free the cross-fade cache; 1 if there was one
int Sprite_FreeMix(Sprite_t* s)
{
	int had;
	FreeBmpSlot(&s->mixCache, &had);
	return had;
}

/* mode 5 with skew: skewCache = skewed copy of the mix cache (two bitmaps)
 * or of bitmap 1, widened by skewAmount (rounded up to an even number of
 * pixels); dropped and left empty when the source is gone. */
void Sprite_RebuildSkew(Sprite_t* s)
{
	Bmp_t info;
	const Bmp_t* src;
	int32_t w, extra;
	if(s->mode != 5)
		return;
	Sprite_FreeSkew(s);
	if((uint32_t)s->skewOn == 0 || (uint32_t)s->skewAmount == 0)
		return;
	if(s->bmp2 != -1)
	{
		if(!s->mixCache.pixels)
			return;
		src = &s->mixCache;
	}
	else
	{
		if(!SpCurrent(&info, s->bmp1, s->gen1))
			return;
		src = &info;
	}
	w = src->w;
	extra = (int32_t)(((uint32_t)(s->skewAmount + 0x10000) * (uint32_t)w) >> 16) - w;
	extra = (extra + 1) & ~1;
	Bmp_Alloc(&s->skewCache, w + extra, src->h, src->mode);
	Blit_Skew(&s->skewCache, src, s->skewOn, s->skewArg, s->skewAmount);
}

// free the skew cache; 1 if there was one
int Sprite_FreeSkew(Sprite_t* s)
{
	int had;
	FreeBmpSlot(&s->skewCache, &had);
	return had;
}

void Sprite_FreeRingTable(Sprite_t* s)
{
	BGI_Free(s->ringTable);
	s->ringTable = NULL;
}

// free the private wipe pattern; 1 if there was one
int Sprite_FreeWipeGray(Sprite_t* s)
{
	int had;
	FreeBmpSlot(&s->wipeGray, &had);
	return had;
}

// ========================================================================
// geometry
// ========================================================================

/* floor / ceil as the original computes them: the fraction is detected on
 * the value scaled by 65536 and truncated */
static int32_t Floor16(double v)
{
	if(v < 0.0)
		return BGI_Ftol(v) - ((((uint32_t)BGI_Ftol(v * 65536.0) & 0xffff) + 0xffff) >> 16);
	return BGI_Ftol(v);
}

static int32_t Ceil16(double v)
{
	if(v > 0.0 || v != v)
		return BGI_Ftol(v) + (int32_t)((((uint32_t)BGI_Ftol(v * 65536.0) & 0xffff) + 0xffff) >> 16);
	return BGI_Ftol(v);
}

/* The axis-aligned box of a w x h picture widened by `skew`, scaled by
 * (sx, sy) and rotated by `angle` about the origin (cx, cy) (all 16.16);
 * the fraction of the scaled 16.16 position (fx, fy) extends the box by
 * up to a pixel.  Writes the box size and `anchor`, where the origin lands
 * inside the box (so the box is drawn at position - anchor). */
void Sprite_BoundsProjected(int32_t* outW, int32_t* outH, int32_t anchor[2], int w, int h, int32_t skew,
	int32_t cx, int32_t cy, int32_t angle, int32_t sx, int32_t sy,
	int32_t fx, int32_t fy)
{
	double Wd, Hd, CX, CY, theta, c, s_, SX, SY, X[4], Y[4], RX[4], RY[4];
	double minX = 1e9, maxX = -1e9, maxY = -1e9, minY = 1e9, A, B;
	uint32_t fracX, fracY;
	int32_t X0, X1, Y0, Y1;
	int i;

	// the picture widened by the skew, in pixels
	Wd = (double)(uint32_t)((uint32_t)(skew + 0x10000) * (uint32_t)w) * INV_65536;
	Hd = (double)(uint32_t)h;
	// the origin, shifted right by half the extra width
	CX = ((Wd - (double)(uint32_t)w) * 0.5) + (double)cx * INV_65536;
	CY = (double)cy * INV_65536;
	theta = (double)angle * DEG16_TO_RAD;
	c = cos(theta);
	s_ = sin(theta);
	SX = (double)(uint32_t)sx * INV_65536;
	SY = (double)(uint32_t)sy * INV_65536;

	// the four corners relative to the origin, y pointing up; the 0.0001
	// keeps a corner exactly on a pixel boundary from adding a pixel
	X[0] = -CX;
	Y[0] = CY;
	X[1] = Wd - CX - 0.0001;
	Y[1] = CY;
	X[2] = -CX;
	Y[2] = CY - Hd + 0.0001;
	X[3] = Wd - CX - 0.0001;
	Y[3] = CY - Hd + 0.0001;
	for(i = 0; i < 4; i++)
	{
		double xs = SX * X[i], ys = SY * Y[i];
		RX[i] = xs * c - ys * s_;
		RY[i] = ys * c + xs * s_;
	}
	for(i = 0; i < 4; i++)
	{
		if(minX > RX[i])
			minX = RX[i];
		if(maxX < RX[i])
			maxX = RX[i];
		if(maxY < RY[i])
			maxY = RY[i];
		if(minY > RY[i])
			minY = RY[i];
	}
	// the fractional part of the scaled position extends the box
	fracX = (uint32_t)MulShr(fx, (uint32_t)sx, 16) & 0xffff;
	fracY = (uint32_t)MulShr(fy, (uint32_t)sy, 16) & 0xffff;
	A = (double)fracX * INV_65536 + maxX;
	B = minY - (double)fracY * INV_65536;

	X0 = Floor16(minX);
	X1 = Floor16(A);
	Y1 = Ceil16(maxY);
	Y0 = Ceil16(B);
	*outW = X1 - X0 + 1;
	*outH = Y1 - Y0 + 1;
	anchor[0] = -X0;
	anchor[1] = Y1;
}

// the same without a position (mode 2)
void Sprite_BoundsTransform(int32_t* outW, int32_t* outH, int32_t anchor[2], int w, int h, int32_t skew,
	int32_t cx, int32_t cy, int32_t angle, int32_t sx, int32_t sy)
{
	Sprite_BoundsProjected(outW, outH, anchor, w, h, skew, cx, cy, angle, sx, sy, 0, 0);
}

/* The transform in effect at the current progress: the base transform
 * (origin, angle, scales) plus the deltas of setParam 0x80..0x82 scaled by
 * the progress - the origin and the scales linearly, the angle through
 * the easing curve of setParam 0x8F - and then the zoom of setParam 0x42
 * multiplied into the scales. */
void Sprite_Animate(Sprite_t* s, int32_t outCentre[2], int32_t* outAngle, int32_t* outSx, int32_t* outSy,
	int32_t ox, int32_t oy, int32_t angle, int32_t sx, int32_t sy)
{
	uint32_t t = (uint32_t)DispObj_GetProgress(&s->obj, 1);
	outCentre[0] = ox + MulShr(s->dOx, t, 24);
	outCentre[1] = oy + MulShr(s->dOy, t, 24);
	*outAngle = angle + (int32_t)(((int64_t)Ease((int32_t)t, s->curve) * (int64_t)s->dAngle) >> 16);
	*outSx = sx + MulShr(s->dSx, t, 24);
	*outSy = sy + MulShr(s->dSy, t, 24);
	if(s->zoomX != 0x10000) // (after the deltas)
		*outSx = (int32_t)(((int64_t)*outSx * s->zoomX) >> 16);
	if(s->zoomY != 0x10000)
		*outSy = (int32_t)(((int64_t)*outSy * s->zoomY) >> 16);
}

/* setParam 0x42 (1.494 on): a 16.16 zoom multiplied into the animated
 * scale of modes 2 and 5; ignored in the other modes.  A zero zx becomes
 * 1 (not 1.0), a zero zy means "the same as zx".  Every mode set resets
 * the zoom to 1.0. */
void Sprite_SetZoom(Sprite_t* s, int32_t zx, int32_t zy)
{
	if(s->mode != 2 && s->mode != 5)
		return;
	if(zx == 0)
		zx = 1;
	s->zoomX = zx;
	s->zoomY = zy ? zy : zx;
	if(s->mode == 2)
		Sprite_RecalcTransform(s);
	else
	{
		Sprite_ProjectPosition(s);
		Sprite_RecalcProjected(s);
	}
}

// mode 2: recompute the current transform and the box; a box under 2 x 2 or a vanished bitmap leaves everything as it was
void Sprite_RecalcTransform(Sprite_t* s)
{
	Bmp_t b;
	int32_t centre[2], cAngle, cSx, cSy, outW, outH, anchor[2];
	if(!SpCurrent(&b, s->bmp1, s->gen1))
		return;
	Sprite_Animate(s, centre, &cAngle, &cSx, &cSy, s->ox, s->oy, s->angle, s->sx, s->sy);
	Sprite_BoundsTransform(&outW, &outH, anchor, b.w, b.h, 0, centre[0], centre[1], cAngle, cSx, cSy);
	if((uint32_t)outW < 2 || (uint32_t)outH < 2)
		return;
	s->outH = outH;
	s->curOx = centre[0];
	s->curOy = centre[1];
	s->anchorX = anchor[0];
	s->anchorY = anchor[1];
	s->curAngle = cAngle;
	s->curSx = cSx;
	s->curSy = cSy;
	s->outW = outW;
	s->obj.vt->setSize(&s->obj, outW, outH);
}

// mode 5: the same with the perspective factor of the depth as the scale, and the position re-projected
void Sprite_RecalcProjected(Sprite_t* s)
{
	Bmp_t b;
	int32_t fp[4], scale, centre[2], cAngle, cSx, cSy, outW, outH, anchor[2], skew;
	if(!SpGetInfo(&b, s->bmp1))
		return;
	DispObj_EffFixedPos(&s->obj, fp); // with the "91 37" / "92 37" offsets of 1.69/472 on
	ProjectScale(&scale, fp[2], SpProjDist(s));
	Sprite_Animate(s, centre, &cAngle, &cSx, &cSy, s->ox, s->oy, s->angle, scale, scale);
	skew = (uint32_t)s->skewOn > 0 ? s->skewAmount : 0;
	Sprite_BoundsProjected(&outW, &outH, anchor, b.w, b.h, skew, centre[0], centre[1], cAngle, cSx, cSy,
		fp[0], fp[1]);
	if((uint32_t)outW < 2 || (uint32_t)outH < 2)
		return;
	s->curOx = centre[0];
	s->curOy = centre[1];
	s->curAngle = cAngle;
	s->curSx = cSx;
	s->curSy = cSy;
	s->outW = outW;
	s->outH = outH;
	s->anchorX = anchor[0];
	s->anchorY = anchor[1];
	Sprite_ProjectPosition(s);
	s->obj.vt->setSize(&s->obj, outW, outH);
}

/* mode 5: the 16.16 world position (origin = the vanishing point, the
 * screen centre) becomes the integer pixel position plus a sub-pixel
 * remainder (subX, subY) that the draw adds back.  With projPos the
 * position is scaled by the perspective factor as well. */
void Sprite_ProjectPosition(Sprite_t* s)
{
	Bmp_t back;
	int32_t fp[4], x, y, c[2];
	DispObj_EffFixedPos(&s->obj, fp); // with the "91 37" / "92 37" offsets of 1.69/472 on
	Gfx_GetBackBmp(&back);
	// the vanishing point: the centre of the back buffer, or from 1.535 on
	// the "90 06" point for the manager's sprites
	c[0] = (int32_t)((uint32_t)back.w << 15);
	c[1] = (int32_t)((uint32_t)back.h << 15);
	if(s->useProjCentre && Gfx_GetProjCentre(gDispGfx, c))
	{
		c[0] = (int32_t)((uint32_t)c[0] << 16);
		c[1] = (int32_t)((uint32_t)c[1] << 16);
	}
	if(s->projPos)
	{
		int32_t k;
		ProjectScale(&k, fp[2], SpProjDist(s));
		x = MulShr(fp[0], (uint32_t)k, 16) + c[0];
		y = MulShr(fp[1], (uint32_t)k, 16) + c[1];
	}
	else
	{
		x = c[0] + fp[0];
		y = c[1] + fp[1];
	}
	s->obj.vt->setPos(&s->obj, x >> 16, y >> 16);
	s->subX = x & 0xffff;
	s->subY = y & 0xffff;
}

/* The rectangle to dirty for a bitmap-space rectangle `in` ("90 53").
 * Modes 0, 1 and 3 pass it on unchanged (the sprite position is not added:
 * the original offsets by its local rectangle, which starts at 0); modes 2
 * and 4 return 0 (invalidate everything); mode 5 maps the rectangle through
 * the current transform and returns 1.  The mode 5 arithmetic reproduces
 * the original on purpose: it builds the vertical extent from the x
 * position and the x scale (an off-by-one in its stack offsets). */
int Sprite_MapUpdateRect(Sprite_t* s, Rect_t* out, const Rect_t* in)
{
	switch(s->mode)
	{
		case 0:
		case 1:
		case 3:
		{
			Rect_t lr;
			s->obj.vt->localRect(&s->obj, &lr);
			out->r = in->r + lr.l;
			out->b = in->b + lr.t;
			out->l = in->l + lr.l;
			out->t = in->t + lr.t;
			return 1;
		}
		case 5:
		{
			Bmp_t b;
			int32_t fp[4], scale, centre[2], cAngle, cSx, cSy, skew, outW, outH, anchor[2], pos[2], ax;
			if(!SpGetInfo(&b, s->bmp1))
				return 0;
			DispObj_EffFixedPos(&s->obj, fp); // with the "91 37" / "92 37" offsets of 1.69/472 on
			ProjectScale(&scale, fp[2], SpProjDist(s));
			Sprite_Animate(s, centre, &cAngle, &cSx, &cSy, s->ox, s->oy, s->angle, scale, scale);
			centre[0] -= (int32_t)((uint32_t)in->l << 16);
			centre[1] -= (int32_t)((uint32_t)in->t << 16);
			skew = (uint32_t)s->skewOn > 0 ? s->skewAmount : 0;
			Sprite_BoundsProjected(&outW, &outH, anchor, in->r - in->l + 1, in->b - in->t + 1, skew,
				centre[0], centre[1], cAngle, cSx, cSy, fp[0], fp[1]);
			DispObj_GetDrawPos(&s->obj, pos);
			ax = pos[0] - anchor[0];
			out->l = ax - (int32_t)((uint32_t)cSx >> 16);
			out->r = ax + outW - 1;
			ax = pos[0] - anchor[1]; // sic: pos[0], see above
			out->t = ax - (int32_t)((uint32_t)(cSx - 1) >> 17);
			out->b = (int32_t)((uint32_t)cSy >> 17) + ax + cSx - 1;
			return 1;
		}
		default:
			return 0;
	}
}

// ========================================================================
// drawing
// ========================================================================

/* Fetch the mask for the dirty piece `local` (object coordinates).  Returns
 * 1 when masking applies: `mask` is then the "90 55" bitmap (screen
 * coordinates; FinishMasked crops it), or, for a "91 55" link, a GRAY8
 * bitmap of the piece composed from the linked sprite's picture at that
 * sprite's position - that one is owned (*owned = 1) and freed by
 * FinishMasked.  Returns 0 to draw unmasked: no mask, or its bitmap is
 * gone, or the linked sprite's picture is not GRAY8. */
static int SpMask(Sprite_t* s, Bmp_t* mask, const Rect_t* local, int* owned)
{
	*owned = 0;
	if(s->maskObj)
	{
		Sprite_t* m = s->maskObj;
		Bmp_t pic;
		int32_t pos[2], mpos[2];
		int lw = local->r - local->l + 1, lh = local->b - local->t + 1;
		if(!SpCurrent(&pic, m->bmp1, m->gen1) || pic.mode != PM_GRAY8)
			return 0;
		s->obj.vt->getDrawPos(&s->obj, pos);
		m->obj.vt->getDrawPos(&m->obj, mpos);
		Bmp_Alloc(mask, lw, lh, PM_GRAY8);
		memset(mask->pixels, 0, (size_t)mask->pitch * (size_t)mask->h);
		Bmp_Blit(mask, mpos[0] - pos[0] - local->l, mpos[1] - pos[1] - local->t, &pic, 0x80, 0);
		*owned = 1;
		return 1;
	}
	if(!s->maskOn)
		return 0;
	return SpCurrent(mask, s->maskBmp, s->maskGen);
}

/* The shared tail of the masked paths: the part of the mask under the
 * dirty piece becomes the alpha of the temporary `tmp` (which holds the
 * piece already rendered), `tmp` is composited onto `dst` with the effect
 * and level, and both temporaries are freed.  From 1.529 on the "90 55"
 * mask may be smaller than the screen: only the overlap of the piece and
 * the mask is drawn (the original clips the piece to the mask and skips
 * the blit when nothing is left). */
static void FinishMasked(Sprite_t* s, Bmp_t* dst, const Rect_t* local, Bmp_t* tmp, Bmp_t* mask, int owned, int effect,
	int level)
{
	if(owned)
	{
		Blit_ApplyMask(tmp, tmp, mask);
		Bmp_Free(mask);
	}
	else
	{
		Rect_t r = *local;
		int32_t pos[2];
		s->obj.vt->getDrawPos(&s->obj, pos);
		Rect_Offset(&r, pos[0], pos[1]); // the piece on the screen
		if(gEngine->gen >= GEN_1_529)
		{
			Rect_t mr;
			Bmp_t tv = *tmp, dv = *dst;
			Rect_FromBmp(&mr, mask);
			if(!Rect_Clip(&r, &mr))
			{
				Bmp_Free(tmp);
				return;
			}
			Bmp_Crop(mask, &r);
			Rect_Offset(&r, -pos[0] - local->l, -pos[1] - local->t); // the overlap inside the piece
			Bmp_Crop(&tv, &r);
			Bmp_Crop(&dv, &r);
			Blit_ApplyMask(&tv, &tv, mask);
			Bmp_BlitEffect(&dv, &tv, effect, level);
			Bmp_Free(tmp);
			return;
		}
		Bmp_Crop(mask, &r);
		Blit_ApplyMask(tmp, tmp, mask);
	}
	Bmp_BlitEffect(dst, tmp, effect, level);
	Bmp_Free(tmp);
}

/* Paint the piece `local` (object coordinates) of the sprite into `dst`,
 * which is already cropped to it.  A mode whose parameters make it look
 * like a cheaper one is drawn as that one; a masked piece, or an effect
 * the mode's blitter cannot apply, is rendered into a temporary first.
 * Nothing is drawn when a bitmap the sprite was set with has since been
 * freed or reloaded (its generation changed). */
static void Sprite_Draw(DispObj_t* o, Bmp_t* dst, const Rect_t* local, uint32_t minKey)
{
	Sprite_t* s = (Sprite_t*)o;
	int effect = DispObj_GetEffect(o);
	int level = DispObj_EffectiveLevel(o);
	int prog = DispObj_GetProgressInt(o);
	int mode = s->mode;
	int lw = local->r - local->l + 1, lh = local->b - local->t + 1;
	Bmp_t a, b, mask, tmp;
	int maskOwned;
	(void)minKey;

	// cheaper modes that paint the same pixels: an untransformed mode 2 is
	// mode 0, a finished wipe is mode 0, an untransformed and unskewed
	// mode 5 is mode 0 or (with a second bitmap) mode 1
	if(mode == 2)
	{
		if(s->curAngle == 0 && s->curSx == 0x10000 && s->curSy == 0x10000)
			mode = 0;
	}
	else if(mode == 3)
	{
		if((uint32_t)prog >= 0x100)
			mode = 0;
	}
	else if(mode == 5)
	{
		if(s->curAngle == 0 && s->curSx == 0x10000 && s->curSy == 0x10000 && s->subX == 0 &&
			s->subY == 0 && !(s->skewOn != 0 && s->skewAmount != 0))
			mode = s->bmp2 != -1;
	}
	else if((uint32_t)mode > 5)
	{
		return;
	}

	switch(mode)
	{
		case 0:
			if(!SpCurrent(&a, s->bmp1, s->gen1))
				return;
			if(s->mode != 3 && SpMask(s, &mask, local, &maskOwned)) // a finished wipe is never masked
			{
				Bmp_Alloc(&tmp, lw, lh, a.mode);
				Bmp_Crop(&a, local);
				Blit_Copy(&tmp, &a);
				FinishMasked(s, dst, local, &tmp, &mask, maskOwned, effect, level);
				return;
			}
			Bmp_Crop(&a, local);
			Bmp_BlitEffect(dst, &a, effect, level);
			return;

		case 1:
			if(!SpGetInfo(&a, s->bmp1) || !SpGetInfo(&b, s->bmp2))
				return;
			if(SpGen(s->bmp1) != s->gen1 || SpGen(s->bmp2) != s->gen2)
				return;
			if(SpMask(s, &mask, local, &maskOwned))
			{
				Bmp_Alloc(&tmp, lw, lh, a.mode);
				Bmp_Crop(&a, local);
				Bmp_Crop(&b, local);
				Blit_CrossfadeTo(&tmp, &a, &b, s->mixRatio);
				FinishMasked(s, dst, local, &tmp, &mask, maskOwned, effect, level);
				return;
			}
			Bmp_Crop(&a, local);
			Bmp_Crop(&b, local);
			if(!DirectEffect(effect))
			{
				Bmp_Alloc(&tmp, a.w, a.h, a.mode);
				Blit_CrossfadeTo(&tmp, &a, &b, s->mixRatio);
				Bmp_BlitEffect(dst, &tmp, effect, level);
				Bmp_Free(&tmp);
				return;
			}
			Blit_Crossfade(dst, &a, &b, s->mixRatio, effect ? level : 0); // effect 0 (copy): opaque
			return;

		case 2:
		{
			// the origin lands `anchor` into the box; the piece starts `local` into it
			int32_t x16 = (int32_t)((uint32_t)(s->anchorX - local->l) << 16);
			int32_t y16 = (int32_t)((uint32_t)(s->anchorY - local->t) << 16);
			if(!SpCurrent(&a, s->bmp1, s->gen1))
				return;
			if(SpMask(s, &mask, local, &maskOwned))
			{
				Bmp_Alloc(&tmp, lw, lh, a.mode);
				Blit_XformCopy(&tmp, x16, y16, &a, s->curOx, s->curOy, s->curAngle, s->curSx, s->curSy, 0, s->smooth);
				FinishMasked(s, dst, local, &tmp, &mask, maskOwned, effect, level);
				return;
			}
			if(!DirectEffect(effect))
			{
				Bmp_Alloc(&tmp, lw, lh, a.mode);
				Blit_XformCopy(&tmp, x16, y16, &a, s->curOx, s->curOy, s->curAngle, s->curSx, s->curSy, 0, s->smooth);
				Bmp_BlitEffect(dst, &tmp, effect, level);
				Bmp_Free(&tmp);
				return;
			}
			Blit_Xform(dst, x16, y16, &a, s->curOx, s->curOy, s->curAngle, s->curSx, s->curSy, level, s->smooth);
			return;
		}

		case 3: // the wipe is never masked
			if(!SpCurrent(&a, s->bmp1, s->gen1))
				return;
			b = s->wipeGray;
			Bmp_Crop(&a, local);
			Bmp_Crop(&b, local);
			if(!DirectEffect(effect))
			{
				Bmp_Alloc(&tmp, a.w, a.h, a.mode);
				Blit_GrayWipeTo(&tmp, &a, &b, s->wipeMode, prog);
				Bmp_BlitEffect(dst, &tmp, effect, level);
				Bmp_Free(&tmp);
				return;
			}
			Blit_GrayWipe(dst, &a, &b, s->wipeMode, prog, level);
			return;

		case 4: // the ripple reads the whole bitmap (the displacement may reach outside the piece) and draws with the fade as its level
		{
			Bmp_t crop;
			if(!SpCurrent(&a, s->bmp1, s->gen1))
				return;
			if(!SpCurrent(&b, s->rippleMap, s->rippleMapGen))
				return;
			crop = a;
			Bmp_Crop(&crop, local);
			Bmp_Crop(&b, local);
			Blit_RippleLevel(dst, &crop, &a, &b, s->ringTable, o->fade);
			return;
		}

		case 5:
		{
			const Bmp_t* src;
			// as in mode 2, plus the sub-pixel remainder of the projected position
			int32_t x16 = (int32_t)(((uint32_t)(s->anchorX - local->l) << 16) + (uint32_t)s->subX);
			int32_t y16 = (int32_t)(((uint32_t)(s->anchorY - local->t) << 16) + (uint32_t)s->subY);
			int32_t cx = s->curOx, cy = s->curOy;
			if(s->skewOn != 0 && s->skewAmount != 0)
			{
				if(!SpGetInfo(&a, s->bmp1))
					return;
				// the skew widened the picture: move the origin by half of it
				cx += (int32_t)(((uint32_t)(s->skewCache.w - a.w) << 15) & 0xffff0000u);
				src = &s->skewCache;
			}
			else if(s->bmp2 != -1)
			{
				src = &s->mixCache;
			}
			else
			{
				if(!SpCurrent(&a, s->bmp1, s->gen1))
					return;
				src = &a;
			}
			if(!src->pixels)
				return; // a cache that could not be built
			if(SpMask(s, &mask, local, &maskOwned))
			{
				Bmp_Alloc(&tmp, lw, lh, src->mode);
				Blit_XformCopy(&tmp, x16, y16, src, cx, cy, s->curAngle, s->curSx, s->curSy, 0, s->smooth);
				FinishMasked(s, dst, local, &tmp, &mask, maskOwned, effect, level);
				return;
			}
			if(DirectEffect(effect) && dst->mode == PM_RGB32)
			{
				Blit_Xform(dst, x16, y16, src, cx, cy, s->curAngle, s->curSx, s->curSy, level, s->smooth);
				return;
			}
			Bmp_Alloc(&tmp, lw, lh, src->mode);
			Blit_XformCopy(&tmp, x16, y16, src, cx, cy, s->curAngle, s->curSx, s->curSy, 0, s->smooth);
			Bmp_BlitEffect(dst, &tmp, effect, level);
			Bmp_Free(&tmp);
			return;
		}

		default:
			return;
	}
}

const DispObjVtbl_t Sprite_Vtbl = {
	Sprite_Destroy,
	DispObj_SetVisible,
	DispObj_IsVisible,
	DispObj_Invalidate,
	Sprite_Draw,
	DispObj_SortKey,
	DispObj_LocalRect,
	DispObj_ScreenRect,
	DispObj_SetPosEx,
	DispObj_SetPos,
	DispObj_GetPos,
	Sprite_GetDrawPos,
	DispObj_SetOffset,
	Sprite_SetFixedPos,
	DispObj_GetFixedPos,
	Sprite_SetLevel,
	Sprite_GetLevel,
	Sprite_SetProgress,
	Sprite_SetParam,
	DispObj_HitTest,
	DispObj_NopNotify,
	DispObj_BuildCache,
	DispObj_SetSize,
	Sprite_GetParam,
};
