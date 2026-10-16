/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dispobj.c - DispObj, the base class of every display object.  Interface in
 * inc/bgi/gfx/dispobj.h, which describes every field.
 *
 * What lives here: position (integer, 16.16 fixed and the draw offsets that
 * are added at draw time), visibility, level / fade / opacity and the
 * effective level the blitters get, the effect mode, the sort key the
 * compositor orders by, the optional 1-bit hit mask, the owner / children
 * tree through which a group moves its members, the parameter switches of
 * "90 38" / "91 38", and DispObj_DrawClipped, the wrapper through which the
 * compositor calls every object's draw method.  The two numeric helpers
 * Ease and ProjectScale are shared with the sprite, background and
 * effector classes.
 *
 * Subclasses embed a DispObj_t as their first member, copy DispObj_Vtbl and
 * override the slots they need; the base implementations below forward to
 * the children where the original does (visibility, level, progress,
 * position).
 */
#include <math.h>
#include "bgi/gfx/dispobj.h"
#include "bgi/version.h" // the parameters the later builds added

Gfx_t* gDispGfx;        // the graphics manager the objects report dirty rectangles to
BmpMgr_t* gDispBmpMgr;  // the bitmap manager the objects fetch their pictures from
int gGlobalEffectValue; // "90 08": a value the scripts set; stored only

void DispObj_SetGfxPtr(Gfx_t* g)
{
	gDispGfx = g;
}

void DispObj_SetBmpMgrPtr(BmpMgr_t* m)
{
	gDispBmpMgr = m;
}

void Gfx_SetGlobalEffectValue(int v)
{
	gGlobalEffectValue = v;
}

// the back buffer descriptor of the graphics manager
void Gfx_GetBackBmp(Bmp_t* out)
{
	Gfx_CopyBackBmp(gDispGfx, out);
}

// ========================================================================
// construction
// ========================================================================

/* Initialise an already allocated object: enabled, invisible, flat (ordered
 * by creation), effect 0x80, level 0, opacity 0x100, no mask, no owner.  The
 * slot id is the manager's creation counter and only its low 15 bits are
 * kept (they become the low half of the sort key).  The pixel holder is
 * allocated here and freed by the destructor. */
void DispObj_Ctor(DispObj_t* o, int classOrder, int slotId)
{
	o->vt = &DispObj_Vtbl;
	o->slotId = slotId & 0x7fff;
	o->classOrder = classOrder;
	o->owner = NULL;
	o->proc = NULL;
	o->pad84[0] = 0;
	o->children = NULL;
	DispObj_SetEnabled(o, 1);
	DispObj_SetVisible(o, 0);
	DispObj_SetFlat(o, 1);
	DispObj_SetSnap(o, 0, 0);
	o->noPerspective = 0;
	o->followOffset = 1;
	o->paramC5 = 1;
	o->sortMode = 0;
	o->flagBits = 0xffffffffu;
	o->param_1 = 0;
	memset(o->fixOff1, 0, sizeof o->fixOff1);
	memset(o->fixOff2, 0, sizeof o->fixOff2);
	o->sortBias = 0;
	o->redrawEveryFrame = 0;
	o->off2X = o->off2Y = 0;
	o->opacity = 0x100;
	memset(o->userValues, 0, sizeof o->userValues);
	DispObj_SetFixedPos(o, 0, 0, 0);
	DispObj_SetOffset(o, 0, 0);
	DispObj_SetEffect(o, 0x80);
	DispObj_SetLevel(o, 0);
	DispObj_SetFade(o, 0);
	DispObj_SetProgressInt(o, 0);
	DispObj_SetPriority(o, 0);
	o->cacheBuf = (PixBuf_t*)BGI_Alloc(sizeof(PixBuf_t));
	PixBuf_Ctor(o->cacheBuf);
	memset(&o->bmp, 0, sizeof o->bmp);
	o->hitEnabled = 1;
	o->maskW = o->maskH = o->maskPitch = 0;
	o->maskBits = NULL;
}

/* The destructor body: frees the pixel holder and the hit mask, detaches
 * every child and takes the object out of its owner's list.  Subclass
 * destructors call this last. */
void DispObj_Dtor(DispObj_t* o)
{
	o->vt = &DispObj_Vtbl;
	if(o->cacheBuf)
	{
		PixBuf_Free(o->cacheBuf);
		BGI_Free(o->cacheBuf);
	}
	DispObj_BuildHitMask(o, NULL);
	while(o->children)
		DispObj_DetachChild(o, o->children->obj);
	if(o->owner)
		DispObj_DetachChild(o->owner, o);
}

// vtable slot 0: destroy, and free the memory when bit 0 of `flags` is set
void DispObj_Destroy(DispObj_t* o, int flags)
{
	DispObj_Dtor(o);
	if(flags & 1)
		BGI_Free(o);
}

// ========================================================================
// numeric helpers
// ========================================================================

/* The easing curves of the animations: maps a progress t (8.16,
 * 0x1000000 = 1.0) to a 16.16 factor.
 *   0 (and any number outside 1..15)  linear
 *   1  ease in-out (a half cosine)
 *   2  ease out (a quarter sine)
 *   3  ease in
 *   4..15  power curves with the exponents 2, 2.5, 3, 4, 5, 6, two per
 *          exponent: the even number eases in (t^n), the odd one eases out
 *          (1 - (1 - t)^n)
 * The trigonometric curves work in 16.16 degrees; the power curves divide
 * pow(t, n) by pow(2^24, n). */
int32_t Ease(int32_t t, int curve)
{
	static const double powers[6] = {2.0, 2.5, 3.0, 4.0, 5.0, 6.0};
	const double DEG16_TO_RAD = 2.663161090079238e-07;
	int32_t a;
	switch(curve)
	{
		case 1: // ease in-out: (cos(180deg - x*180) + 1) / 2
			a = (int32_t)((int64_t)t * 180 / 256);
			return BGI_Ftol((cos((double)(0xb40000 - a) * DEG16_TO_RAD) + 1.0) * 32768.0);
		case 2: // ease out: sin(x * 90)
			a = (int32_t)((int64_t)t * 90 / 256);
			return BGI_Ftol(sin((double)a * DEG16_TO_RAD) * 65536.0);
		case 3: // ease in: 1 - sin(90 - x * 90)
			a = (int32_t)((int64_t)t * 90 / 256);
			return BGI_Ftol((1.0 - sin((double)(0x5a0000 - a) * DEG16_TO_RAD)) * 65536.0);
		default:
			if(curve >= 4 && curve <= 15)
			{
				double n = powers[(curve - 4) / 2];
				if(curve & 1) // 1 - (1 - x)^n
					return BGI_Ftol((1.0 - pow((double)(0x1000000 - t), n) / pow(16777216.0, n)) * 65536.0);
				return BGI_Ftol(pow((double)t, n) * 65536.0 / pow(16777216.0, n));
			}
			return t / 256; // linear: 8.16 -> 16.16
	}
}

/* The 16.16 perspective factor for a depth z (16.16) at the projection
 * distance `dist`: 1.0 at the screen plane (or when either value is 0),
 * dist / (dist + z) behind it, and growing linearly in front of it. */
void ProjectScale(int32_t* out, int32_t z, int32_t dist)
{
	if(z == 0 || dist == 0)
	{
		*out = 0x10000;
	}
	else if(z >= 0)
	{ // behind the screen plane: shrink
		*out = (int32_t)(((int64_t)dist * 0x100000000LL) / (((int64_t)(uint32_t)dist << 16) + z));
	}
	else
	{ // in front: grow linearly
		*out = (int32_t)((((int64_t)(uint32_t)dist << 16) - z) / dist);
	}
}

// ========================================================================
// tree
// ========================================================================

/* Make `child` follow this object at the offset (dx, dy): the child is moved
 * there at once and again whenever this object moves ("90 E8" adds an
 * object to a group, "91 3E" attaches any object to any other).  Returns
 * 1, or 0 when the child already has an owner. */
int DispObj_AttachChild(DispObj_t* o, DispObj_t* child, int dx, int dy)
{
	ChildNode_t* n;
	int32_t p[2];
	if(child->owner)
		return 0;
	n = (ChildNode_t*)BGI_Alloc(sizeof *n);
	n->obj = child;
	n->dx = dx;
	n->dy = dy;
	n->next = o->children;
	o->children = n;
	child->owner = o;
	o->vt->getPos(o, p);
	child->vt->setPosEx(child, p[0] + dx, p[1] + dy, 0);
	return 1;
}

// take `child` out of the list ("90 E9", "91 3F"); 1 if it was found, 0 otherwise
int DispObj_DetachChild(DispObj_t* o, DispObj_t* child)
{
	ChildNode_t **link = &o->children, *n;
	for(n = o->children; n; link = &n->next, n = n->next)
	{
		if(n->obj == child)
		{
			*link = n->next;
			child->owner = NULL;
			BGI_Free(n);
			return 1;
		}
	}
	return 0;
}

/* Claim the object for `owner` without putting it into a child list (the
 * knob does this with its thumb).  Returns 0 when the object already has an
 * owner, else 1. */
int DispObj_SetOwner(DispObj_t* o, DispObj_t* owner)
{
	if(o->owner)
		return 0;
	o->owner = owner;
	return 1;
}

// the reverse of DispObj_SetOwner; 0 when `owner` is not the current owner
int DispObj_ClearOwner(DispObj_t* o, DispObj_t* owner)
{
	if(o->owner != owner)
		return 0;
	o->owner = NULL;
	return 1;
}

/* Occupy the object's procedure slot (an ObjProc, see sys/objproc.c);
 * sprites and windows refuse to be deleted while one is attached.  0 when
 * the slot is taken, else 1. */
int DispObj_SetProc(DispObj_t* o, void* proc)
{
	if(o->proc)
		return 0;
	o->proc = proc;
	return 1;
}

// release the procedure slot; 0 when `proc` is not the one attached
int DispObj_ClearProc(DispObj_t* o, void* proc)
{
	if(o->proc != proc)
		return 0;
	o->proc = NULL;
	return 1;
}

// after a child moved on its own, remember its new offset; 1 if `child` was found
int DispObj_UpdateChildOffset(DispObj_t* o, DispObj_t* child)
{
	ChildNode_t* n;
	for(n = o->children; n; n = n->next)
	{
		if(n->obj == child)
		{
			int32_t po[2], pc[2];
			o->vt->getPos(o, po);
			child->vt->getPos(child, pc);
			n->dx = pc[0] - po[0];
			n->dy = pc[1] - po[1];
			return 1;
		}
	}
	return 0;
}

// ========================================================================
// plain fields
// ========================================================================

void DispObj_SetEnabled(DispObj_t* o, int f)
{
	o->enabled = f;
}

// the caller re-sorts the object in the compositor afterwards ("90 3A")
void DispObj_SetPriority(DispObj_t* o, uint32_t p)
{
	o->priority = (int32_t)p;
}

uint32_t DispObj_GetPriority(DispObj_t* o)
{
	return (uint32_t)o->priority;
}

void DispObj_SetEffect(DispObj_t* o, int e)
{
	o->effect = e;
}

int DispObj_GetEffect(DispObj_t* o)
{
	return o->effect;
}

// 1: sort by creation order, 0: by depth (the projected sprite switches to 0)
void DispObj_SetFlat(DispObj_t* o, int f)
{
	o->flat = f;
}

/* setParam 0x8000: round the fixed position to whole pixels, see
 * DispObj_SetFixedPos.  1.494 on keep a second value, the mode (1: snap at
 * any depth, 0: only at depth 0). */
void DispObj_SetSnap(DispObj_t* o, int f, int mode)
{
	o->snapToPixel = f;
	o->snapMode = mode;
}

int DispObj_GetLevel(DispObj_t* o)
{
	return o->level;
}

void DispObj_GetPos(DispObj_t* o, int32_t out[2])
{
	out[0] = o->x;
	out[1] = o->y;
}

void DispObj_GetOffset(DispObj_t* o, int32_t out[2])
{
	out[0] = o->offX;
	out[1] = o->offY;
}

// "90 37": the draw offset, added to the position at draw time
void DispObj_SetOffset(DispObj_t* o, int dx, int dy)
{
	o->offX = dx;
	o->offY = dy;
}

int DispObj_HasPixels(DispObj_t* o)
{
	return o->bmp.pixels != NULL;
}

void DispObj_CopyBmp(DispObj_t* o, Bmp_t* out)
{
	*out = o->bmp;
}

// the base class has no cache: 0x80000001 (unsupported)
int DispObj_BuildCache(DispObj_t* o)
{
	return (int)0x80000001;
}

// the base draw and notify methods do nothing
void DispObj_NopDraw(DispObj_t* o, Bmp_t* dst, const Rect_t* local, uint32_t minKey)
{
}
void DispObj_NopNotify(DispObj_t* o, uint32_t what, int a, int b)
{
}

void DispObj_GetFixedPos(DispObj_t* o, int32_t out[4])
{
	out[0] = o->fx;
	out[1] = o->fy;
	out[2] = o->fz;
	out[3] = o->fw;
}

/* "90 34": the fade (0..0x100, an extra transparency), passed on to every
 * child.  1.640 on skip the object itself when bit 0 of its flags (setParam
 * 0xFFFF) is clear; the children are still visited. */
void DispObj_SetFade(DispObj_t* o, int v)
{
	ChildNode_t* n;
	if(gEngine->gen < GEN_1_640 || (o->flagBits & 1))
		o->fade = v;
	for(n = o->children; n; n = n->next)
		DispObj_SetFade(n->obj, v);
}

/* The animation progress, passed on to every child.  `raw` 1 stores `v` as
 * it is (8.16); otherwise `v` is an integer step 0..0x100 ("90 35") and is
 * shifted into the fraction. */
void DispObj_SetProgress(DispObj_t* o, int raw, int v)
{
	ChildNode_t* n;
	o->progress = raw == 1 ? v : v << 16;
	for(n = o->children; n; n = n->next)
		n->obj->vt->setProgress(n->obj, raw, v);
}

// setProgress(0, v): an integer step through the virtual method
void DispObj_SetProgressInt(DispObj_t* o, int v)
{
	o->vt->setProgress(o, 0, v);
}

// `raw` 1: the 8.16 value; else the integer step (the high 16 bits)
int DispObj_GetProgress(DispObj_t* o, int raw)
{
	if(raw == 1)
		return o->progress;
	return (int)(((uint32_t)o->progress >> 16) & 0xffffu);
}

int DispObj_GetProgressInt(DispObj_t* o)
{
	return DispObj_GetProgress(o, 0);
}

/* The level the blitters get: the object's own level combined with the
 * inherited fade according to the effect family.  Effects 0x01 and
 * 0x20..0x24 are transparencies (the opacities multiply), 0x02..0x04, 0xC0
 * and 0xC1 are strengths (the strength shrinks as the object fades);
 * everything else keeps the level unchanged. */
int DispObj_EffectiveLevel(DispObj_t* o)
{
	uint32_t e = (uint32_t)o->effect;
	int cls = 2;
	if(e == 0x01 || (e >= 0x20 && e <= 0x24))
		cls = 0;
	else if((e >= 0x02 && e <= 0x04) || e == 0xc0 || e == 0xc1)
		cls = 1;
	// 1.599 on multiplies the opacity of "90 39" in as well
	switch(cls)
	{
		case 0: return 0x100 - (((0x100 - o->fade) * (0x100 - o->level) * o->opacity) >> 16);
		case 1: return ((0x100 - o->fade) * o->level * o->opacity) >> 16;
		default: return o->level;
	}
}

// "90 39" (1.599 on): the opacity, 0..0x100, passed on to every child
void DispObj_SetOpacity(DispObj_t* o, int v)
{
	ChildNode_t* n;
	o->opacity = v;
	for(n = o->children; n; n = n->next)
		DispObj_SetOpacity(n->obj, v);
}

// ========================================================================
// virtual methods
// ========================================================================

// "90 30" and the class-specific show instructions; passed on to every child
void DispObj_SetVisible(DispObj_t* o, int on)
{
	ChildNode_t* n;
	o->visible = on;
	for(n = o->children; n; n = n->next)
		n->obj->vt->setVisible(n->obj, on);
}

// shown, enabled, not faded out completely and (1.599 on) not fully transparent
int DispObj_IsVisible(DispObj_t* o)
{
	return o->visible && o->enabled && (uint32_t)o->fade < 0x100u && o->opacity > 0;
}

/* Hand the object's screen rectangle and sort key to the compositor as a
 * dirty rectangle, then the children's.  An object without a size
 * (bmp.pitch 0) dirties nothing. */
void DispObj_Invalidate(DispObj_t* o)
{
	ChildNode_t* n;
	if(o->bmp.pitch)
	{
		Rect_t r;
		o->vt->screenRect(o, &r);
		Gfx_AddDirty(gDispGfx, o->vt->sortKey(o), &r);
	}
	for(n = o->children; n; n = n->next)
		n->obj->vt->invalidate(n->obj);
}

/* The key the compositor orders the objects by, ascending: the priority,
 * then the class order, then the creation order (flat objects) or the depth
 * (farther away sorts lower).  1.69/472 adds the bias of setParam 0x8100.
 * 1.640 adds sort mode 1 (setParam 0x8101): the priority, then the depth
 * over its whole 16-bit range, no class order. */
uint32_t DispObj_SortKey(DispObj_t* o)
{
	uint32_t hi;
	if(o->sortMode == 1 && gEngine->gen >= GEN_1_640)
		return ((uint32_t)o->priority << 16) + (uint32_t)(o->sortBias - (o->fz >> 16) + 0x7fff);
	hi = ((uint32_t)(o->priority * 16 + o->classOrder) << 16) + (uint32_t)o->sortBias;
	if(o->flat)
		return hi + (uint32_t)o->slotId;
	// depth order: a larger z (farther away) sorts lower
	return hi + ((0xffff8000u - (uint32_t)(uint16_t)(o->fz >> 16)) & 0xffffu);
}

// the object's rectangle in its own coordinates: {0, 0, w - 1, h - 1}
void DispObj_LocalRect(DispObj_t* o, Rect_t* out)
{
	out->l = 0;
	out->t = 0;
	out->r = o->bmp.w - 1;
	out->b = o->bmp.h - 1;
}

// the local rectangle moved to the draw position
void DispObj_ScreenRect(DispObj_t* o, Rect_t* out)
{
	int32_t p[2];
	o->vt->localRect(o, out);
	o->vt->getDrawPos(o, p);
	Rect_Offset(out, p[0], p[1]);
}

/* Store the position and move the children with it.  With `tellOwner` the
 * owner re-reads this object's offset from the new position (the object
 * moved on its own, as with "90 33"); children moved by their owner get 0. */
void DispObj_SetPosEx(DispObj_t* o, int x, int y, int tellOwner)
{
	ChildNode_t* n;
	o->x = x;
	o->y = y;
	if(tellOwner && o->owner)
		DispObj_UpdateChildOffset(o->owner, o);
	for(n = o->children; n; n = n->next)
		n->obj->vt->setPosEx(n->obj, x + n->dx, y + n->dy, 0);
}

// "90 33": setPosEx with the owner told
void DispObj_SetPos(DispObj_t* o, int x, int y)
{
	o->vt->setPosEx(o, x, y, 1);
}

int32_t gDispGlobalOffset[2]; // "91 06" (1.616 on): added to the draw position of every object that follows it

/* 1.616 on: writes the "91 06" offset to `out` and returns 1 when the object
 * follows it (setParam 0xC4, the default); 0 otherwise, `out` untouched. */
int DispObj_GlobalOffset(DispObj_t* o, int32_t out[2])
{
	if(gEngine->gen < GEN_1_616 || !o->followOffset)
		return 0;
	out[0] = gDispGlobalOffset[0];
	out[1] = gDispGlobalOffset[1];
	return 1;
}

/* Where the object is drawn: the position plus the "90 37" offset; 1.494 adds
 * the second offset of "90 36", 1.616 the global one of "91 06". */
void DispObj_GetDrawPos(DispObj_t* o, int32_t out[2])
{
	int32_t off[2];
	DispObj_GetPos(o, out);
	DispObj_GetOffset(o, off);
	out[0] += off[0] + o->off2X;
	out[1] += off[1] + o->off2Y;
	if(DispObj_GlobalOffset(o, off))
	{
		out[0] += off[0];
		out[1] += off[1];
	}
}

// "90 36" (1.494 on): the second draw offset, passed on to every child
void DispObj_SetOffset2(DispObj_t* o, int dx, int dy)
{
	ChildNode_t* n;
	o->off2X = dx;
	o->off2Y = dy;
	for(n = o->children; n; n = n->next)
		DispObj_SetOffset2(n->obj, dx, dy);
}

/* "91 33": the 16.16 position and depth.  With snapToPixel set, x and y are
 * rounded to whole pixels first - only at depth 0 unless the snap mode is
 * 1 (1.494 on).  A flat object also takes the integer position, as if
 * moved by "90 33". */
void DispObj_SetFixedPos(DispObj_t* o, int32_t fx, int32_t fy, int32_t fz)
{
	if(o->snapToPixel && (fz == 0 || o->snapMode == 1)) // (mode 1: 1.494 on)
	{                                                   // round to whole pixels
		fx = (int32_t)(((uint32_t)fx + 0x8000u) & 0xffff0000u);
		fy = (int32_t)(((uint32_t)fy + 0x8000u) & 0xffff0000u);
	}
	o->fz = fz;
	o->fx = fx;
	o->fy = fy;
	if(o->flat)
		o->vt->setPosEx(o, fx >> 16, fy >> 16, 1);
}

// "90 32": the level, 0..0x100, passed on to every child
void DispObj_SetLevel(DispObj_t* o, int level)
{
	ChildNode_t* n;
	o->level = level;
	for(n = o->children; n; n = n->next)
		n->obj->vt->setLevel(n->obj, level);
}

/* "90 38": set parameter `no` to (a, b).  The base class handles
 *   0           the position (a, b)
 *   1           the effect
 *   2           the level
 *   0x8000      snap to pixel; 1.494 on takes a second value, the snap mode
 *   0x8001      1.494 on: no perspective scaling of a projected sprite
 *   0x8100      1.69/472 on: the sort bias
 *   0x8101      1.640 on: the sort mode, 0 or 1
 *   0xC0, 0xC1  1.573 on: the visible flag and the priority, set directly
 *   0xC4        1.616 on: follow the "91 06" offset (default)
 *   0xC5        1.640 on: a switch, default 1 (stored; its use was not found)
 *   0xFFFF      1.640 on: a = the bits (0: all), b = set or clear them
 *   0x7FFF0000  1.494 on: redraw every frame
 *   0x7FFFFFFF  1.494 on: the 16 script values (a: slot, b: value)
 * Returns 0, 0xffff0001 for a number the build does not support, 0xffff0002
 * for a value out of range.  Subclasses handle their own numbers first and
 * fall back to this. */
int DispObj_SetParam(DispObj_t* o, int no, int a, int b)
{
	switch(no)
	{
		case 0: o->vt->setPos(o, a, b); return 0;
		case 1: DispObj_SetEffect(o, a); return 0;
		case 2: o->vt->setLevel(o, a); return 0;
		case 0xc0:
			if(gEngine->gen < GEN_1_573)
				break;
			o->visible = a;
			return 0;
		case 0xc1:
			if(gEngine->gen < GEN_1_573)
				break;
			o->priority = a;
			return 0;
		case 0xc4:
			if(gEngine->gen < GEN_1_616)
				break;
			o->followOffset = a;
			return 0;
		case 0xc5:
			if(gEngine->gen < GEN_1_640)
				break;
			o->paramC5 = a;
			return 0;
		case 0x8000: DispObj_SetSnap(o, a, gEngine->gen >= GEN_1_494 ? b : 0); return 0;
		case 0x8001:
			if(gEngine->gen < GEN_1_494)
				break;
			o->noPerspective = a;
			return 0;
		case 0x8100: // 1.69/472 on
			if(gEngine->gen < GEN_1_69_472)
				break;
			o->sortBias = a;
			return 0;
		case 0x8101:
			if(gEngine->gen < GEN_1_640)
				break;
			if((uint32_t)a > 1)
				return (int)0xffff0002;
			o->sortMode = a;
			return 0;
		case 0xffff:
		{
			uint32_t mask = a ? (uint32_t)a : 0xffffffffu;
			if(gEngine->gen < GEN_1_640)
				break;
			o->flagBits = b ? (o->flagBits | mask) : (o->flagBits & ~mask);
			return 0;
		}
		case 0x7fff0000: // 1.494 on
			if(gEngine->gen < GEN_1_494)
				break;
			o->redrawEveryFrame = a;
			return 0;
		case 0x7fffffff:
			if(gEngine->gen < GEN_1_494)
				break;
			if((uint32_t)a >= 16)
				return (int)0xffff0002;
			o->userValues[a] = b;
			return 0;
		default: break;
	}
	return (int)0xffff0001; // unsupported parameter
}

// the fixed position with the two offsets of "91 37" / "92 37" added (1.69/472 on)
void DispObj_EffFixedPos(DispObj_t* o, int32_t out[4])
{
	int i;
	o->vt->getFixedPos(o, out);
	for(i = 0; i < 3; i++)
		out[i] += o->fixOff1[i] + o->fixOff2[i];
}

// "91 37" (which 1) / "92 37" or "91 36" (which 2): a 16.16 offset added to the fixed position
void DispObj_SetFixedOffset(DispObj_t* o, int which, int32_t x, int32_t y, int32_t z)
{
	int32_t* v = which == 2 ? o->fixOff2 : o->fixOff1;
	v[0] = x;
	v[1] = y;
	v[2] = z;
}

/* "91 38" (1.69/472 on): read parameter `no` into `out`:
 *   0           the position (two values)
 *   1           the effect
 *   2           the level
 *   3           1.494 on: the priority
 *   0x20        the fixed position (three values)
 *   -1          1.535 on: the value the filter object stores (param_1)
 *   -2          1.616 on: the sort key
 *   0x7FFFFFFF  1.494 on: a script value; out[0] names the slot (0..15)
 *               on entry and receives the value
 * Returns 0, 0xffff0001 for a number the build does not support, 0xffff0002
 * for a slot out of range. */
int DispObj_GetParam(DispObj_t* o, int no, int32_t* out)
{
	int32_t v[4];
	switch(no)
	{
		case 0:
			o->vt->getPos(o, v);
			out[0] = v[0];
			out[1] = v[1];
			return 0;
		case 1: out[0] = o->effect; return 0;
		case 2: out[0] = o->vt->getLevel(o); return 0;
		case 3: // 1.494 on: the priority
			if(gEngine->gen < GEN_1_494)
				break;
			out[0] = o->priority;
			return 0;
		case -1:
			if(gEngine->gen < GEN_1_535)
				break;
			out[0] = o->param_1;
			return 0;
		case -2: // the sort key
			if(gEngine->gen < GEN_1_616)
				break;
			out[0] = (int32_t)o->vt->sortKey(o);
			return 0;
		case 0x20:
			o->vt->getFixedPos(o, v);
			out[0] = v[0];
			out[1] = v[1];
			out[2] = v[2];
			return 0;
		case 0x7fffffff: // 1.494 on: out[0] names the slot and receives the value
			if(gEngine->gen < GEN_1_494)
				break;
			if((uint32_t)out[0] >= 16)
				return (int)0xffff0002;
			out[0] = o->userValues[out[0]];
			return 0;
		default: break;
	}
	return (int)0xffff0001;
}

/* "90 3C": build the 1-bit hit mask from the opaque pixels of `bmp` (16-bit:
 * non-zero, 32-bit: any colour bit, ARGB: any alpha bit, gray: non-zero)
 * and enable hit testing; NULL drops the mask, after which the whole
 * rectangle hits.  The mask keeps its own size: a later change of the
 * object's size does not stretch it. */
void DispObj_BuildHitMask(DispObj_t* o, const Bmp_t* bmp)
{
	int y, x;
	if(o->maskBits)
	{
		BGI_Free(o->maskBits);
		o->hitEnabled = 0;
		o->maskW = o->maskH = o->maskPitch = 0;
		o->maskBits = NULL;
	}
	if(bmp)
	{
		o->maskW = bmp->w;
		o->maskH = bmp->h;
		o->maskPitch = (bmp->w + 7) >> 3;
		o->maskBits = (uint8_t*)BGI_Alloc((size_t)o->maskPitch * (size_t)o->maskH + 1);
		for(y = 0; y < o->maskH; y++)
		{
			const uint8_t* p = bmp->pixels + (size_t)y * bmp->pitch;
			uint8_t* row = o->maskBits + (size_t)y * o->maskPitch;
			memset(row, 0, (size_t)o->maskPitch);
			for(x = 0; x < o->maskW; x++, p += bmp->bpp)
			{
				int hit;
				switch(bmp->mode)
				{
					case PM_RGB16: hit = *(const uint16_t*)p != 0; break;
					case PM_RGB32: hit = (*(const uint32_t*)p & 0xffffffu) != 0; break;
					case PM_ARGB32: hit = (*(const uint32_t*)p & 0xff000000u) != 0; break;
					case PM_GRAY8: hit = *p != 0; break;
					default: hit = 0; break;
				}
				if(hit)
					row[x >> 3] |= (uint8_t)(1 << (x & 7));
			}
		}
	}
	o->hitEnabled = 1;
}

// drop the mask and turn hit testing off altogether
void DispObj_DisableHit(DispObj_t* o)
{
	DispObj_BuildHitMask(o, NULL);
	o->hitEnabled = 0;
}

/* "90 3D": 1 when (x, y), in object coordinates, lies inside the object's
 * rectangle and, if there is a mask, on a set bit of it; 0 otherwise or
 * when hit testing is disabled. */
int DispObj_HitTest(DispObj_t* o, int x, int y)
{
	if(!o->hitEnabled || x < 0 || (uint32_t)x >= (uint32_t)o->bmp.w || y < 0 || (uint32_t)y >= (uint32_t)o->bmp.h)
		return 0;
	if(!o->maskBits)
		return 1;
	if((uint32_t)x >= (uint32_t)o->maskW || (uint32_t)y >= (uint32_t)o->maskH)
		return 0;
	return (o->maskBits[o->maskPitch * y + (x >> 3)] >> (x & 7)) & 1;
}

/* (Re)allocate the pixels of `b` in the holder `buf`: w x h in pixel mode
 * `mode`, tightly packed.  A zero dimension leaves `b` without pixels.
 * Returns 1 when pixels were allocated, 0 otherwise. */
int Bmp_AllocIn(Bmp_t* b, PixBuf_t* buf, int w, int h, int mode)
{
	PixBuf_Free(buf);
	b->w = w;
	b->h = h;
	b->mode = mode;
	b->bpp = ModeBytes(mode);
	b->pitch = b->bpp * w;
	if((uint32_t)w > 0 && (uint32_t)h > 0)
		b->pixels = PixBuf_Alloc(buf, (uint32_t)(b->pitch * h));
	else
		b->pixels = NULL;
	return b->pixels != NULL;
}

/* Bmp_AllocIn with the mode taken from `like`, or, when `like` is NULL, the
 * screen mode with an alpha channel (ARGB for a 32-bit screen). */
int DispObj_AllocLike(DispObj_t* o, Bmp_t* b, PixBuf_t* buf, int w, int h, const Bmp_t* like)
{
	int mode;
	if(like)
		mode = like->mode;
	else
		mode = ScreenMode() == PM_RGB32 ? PM_ARGB32 : ScreenMode();
	return Bmp_AllocIn(b, buf, w, h, mode);
}

/* Give the object a size without pixels: the descriptor gets w, h, the
 * screen pixel mode and a pitch, which is enough for the rectangle and hit
 * tests.  Returns 0 when a dimension is 0, else 1. */
int DispObj_SetSize(DispObj_t* o, int w, int h)
{
	if(w == 0 || h == 0)
		return 0;
	o->bmp.w = w;
	o->bmp.h = h;
	o->bmp.mode = ScreenMode();
	o->bmp.bpp = ModeBytes(o->bmp.mode);
	o->bmp.pitch = o->bmp.bpp * w;
	o->bmp.pixels = NULL;
	return 1;
}

// change the recorded size of an object that has no pixels of its own (the virtual object, the wait icon)
void DispObj_SetVirtualSize(DispObj_t* o, int w, int h)
{
	if(!DispObj_HasPixels(o))
	{
		o->bmp.w = w;
		o->bmp.h = h;
	}
}

// 1 and the size, 0 (outputs untouched) when the object has none
int DispObj_GetSize(DispObj_t* o, int* w, int* h)
{
	if(o->bmp.w == 0 || o->bmp.h == 0)
		return 0;
	*w = o->bmp.w;
	*h = o->bmp.h;
	return 1;
}

// move a screen rectangle into object coordinates
void DispObj_ToLocal(DispObj_t* o, Rect_t* r)
{
	int32_t p[2];
	o->vt->getDrawPos(o, p);
	Rect_Offset(r, -p[0], -p[1]);
}

/* The compositor's way of drawing an object: clip the object's screen
 * rectangle to the screen and to the dirty band, crop the back buffer `g`
 * to that piece and call draw() with the piece in object coordinates (so
 * every draw method may assume dst and `local` have the same size).
 * Returns 1 for an invisible object, otherwise whether the object's screen
 * rectangle lies completely inside `dirty` - the compositor uses that to
 * know when the band holds the whole object. */
int DispObj_DrawClipped(DispObj_t* o, const ScreenBmp_t* g, const Rect_t* dirty, uint32_t minKey)
{
	Rect_t r;
	Bmp_t dst;
	int inside;
	if(!o->vt->isVisible(o))
		return 1;
	o->vt->screenRect(o, &r);
	if(!Rect_Clip(&r, &g->screen))
		return 0;
	inside = Rect_Inside(&r, dirty);
	if(Rect_Clip(&r, dirty))
	{
		dst = g->bmp;
		Bmp_Crop(&dst, &r);
		DispObj_ToLocal(o, &r);
		o->vt->draw(o, &dst, &r, minKey);
	}
	return inside;
}

const DispObjVtbl_t DispObj_Vtbl = {
	DispObj_Destroy,
	DispObj_SetVisible,
	DispObj_IsVisible,
	DispObj_Invalidate,
	DispObj_NopDraw,
	DispObj_SortKey,
	DispObj_LocalRect,
	DispObj_ScreenRect,
	DispObj_SetPosEx,
	DispObj_SetPos,
	DispObj_GetPos,
	DispObj_GetDrawPos,
	DispObj_SetOffset,
	DispObj_SetFixedPos,
	DispObj_GetFixedPos,
	DispObj_SetLevel,
	DispObj_GetLevel,
	DispObj_SetProgress,
	DispObj_SetParam,
	DispObj_HitTest,
	DispObj_NopNotify,
	DispObj_BuildCache,
	DispObj_SetSize,
	DispObj_GetParam,
};
