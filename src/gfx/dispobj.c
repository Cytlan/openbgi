/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dispobj.c - DispObj, the base class of every display object.  Interface in
 * src/include/bgi/gfx/dispobj.h, which describes every field.
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
#include "bgi/core.h"

void DispObj_SetGfxPtr(OpenBGI_t* o, Gfx_t* g)
{
	o->gfx.dispGfx = g;
}

void DispObj_SetBmpMgrPtr(OpenBGI_t* o, BmpMgr_t* m)
{
	o->gfx.dispBmpMgr = m;
}

void Gfx_SetGlobalEffectValue(OpenBGI_t* o, int v)
{
	o->gfx.globalEffectValue = v;
}

// the back buffer descriptor of the graphics manager
void Gfx_GetBackBmp(OpenBGI_t* o, Bmp_t* out)
{
	Gfx_CopyBackBmp(o->gfx.dispGfx, out);
}

// ========================================================================
// construction
// ========================================================================

/* Initialise an already allocated object: enabled, invisible, flat (ordered
 * by creation), effect 0x80, level 0, opacity 0x100, no mask, no owner.  The
 * slot id is the manager's creation counter and only its low 15 bits are
 * kept (they become the low half of the sort key).  The pixel holder is
 * allocated here and freed by the destructor. */
void DispObj_Ctor(OpenBGI_t* o, DispObj_t* obj, int classOrder, int slotId)
{
	obj->vt = &DispObj_Vtbl;
	obj->slotId = slotId & 0x7fff;
	obj->classOrder = classOrder;
	obj->owner = NULL;
	obj->proc = NULL;
	obj->pad84[0] = 0;
	obj->children = NULL;
	obj->hidden = obj->enabledToChildren = obj->hiddenToChildren = 0;
	DispObj_SetEnabled(obj, 1);
	DispObj_SetVisible(o, obj, 0);
	DispObj_SetFlat(obj, 1);
	DispObj_SetSnap(obj, 0, 0);
	obj->noPerspective = 0;
	obj->followOffset = 1;
	obj->paramC5 = 1;
	obj->sortMode = 0;
	obj->flagBits = 0xffffffffu;
	obj->param_1 = 0;
	memset(obj->fixOff1, 0, sizeof obj->fixOff1);
	memset(obj->fixOff2, 0, sizeof obj->fixOff2);
	obj->sortBias = 0;
	obj->redrawEveryFrame = 0;
	obj->off2X = obj->off2Y = 0;
	obj->opacity = 0x100;
	memset(obj->userValues, 0, sizeof obj->userValues);
	DispObj_SetFixedPos(o, obj, 0, 0, 0);
	DispObj_SetOffset(o, obj, 0, 0);
	DispObj_SetEffect(obj, 0x80);
	DispObj_SetLevel(o, obj, 0);
	DispObj_SetFade(o, obj, 0);
	DispObj_SetProgressInt(o, obj, 0);
	DispObj_SetPriority(obj, 0);
	obj->cacheBuf = (PixBuf_t*)BGI_Alloc(sizeof(PixBuf_t));
	PixBuf_Ctor(obj->cacheBuf);
	memset(&obj->bmp, 0, sizeof obj->bmp);
	obj->hitEnabled = 1;
	obj->maskW = obj->maskH = obj->maskPitch = 0;
	obj->maskBits = NULL;
}

/* The destructor body: frees the pixel holder and the hit mask, detaches
 * every child and takes the object out of its owner's list.  Subclass
 * destructors call this last. */
void DispObj_Dtor(DispObj_t* obj)
{
	obj->vt = &DispObj_Vtbl;
	if(obj->cacheBuf)
	{
		PixBuf_Free(obj->cacheBuf);
		BGI_Free(obj->cacheBuf);
	}
	DispObj_BuildHitMask(obj, NULL);
	while(obj->children)
		DispObj_DetachChild(obj, obj->children->obj);
	if(obj->owner)
		DispObj_DetachChild(obj->owner, obj);
}

// vtable slot 0: destroy, and free the memory when bit 0 of `flags` is set
void DispObj_Destroy(OpenBGI_t* o, DispObj_t* obj, int flags)
{
	DispObj_Dtor(obj);
	if(flags & 1)
		BGI_Free(obj);
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
int DispObj_AttachChild(OpenBGI_t* o, DispObj_t* obj, DispObj_t* child, int dx, int dy)
{
	ChildNode_t* n;
	int32_t p[2];
	if(child->owner)
		return 0;
	n = (ChildNode_t*)BGI_Alloc(sizeof *n);
	n->obj = child;
	n->dx = dx;
	n->dy = dy;
	n->next = obj->children;
	obj->children = n;
	child->owner = obj;
	obj->vt->getPos(o, obj, p);
	child->vt->setPosEx(o, child, p[0] + dx, p[1] + dy, 0);
	return 1;
}

// take `child` out of the list ("90 E9", "91 3F"); 1 if it was found, 0 otherwise
int DispObj_DetachChild(DispObj_t* obj, DispObj_t* child)
{
	ChildNode_t **link = &obj->children, *n;
	for(n = obj->children; n; link = &n->next, n = n->next)
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
int DispObj_SetOwner(DispObj_t* obj, DispObj_t* owner)
{
	if(obj->owner)
		return 0;
	obj->owner = owner;
	return 1;
}

// the reverse of DispObj_SetOwner; 0 when `owner` is not the current owner
int DispObj_ClearOwner(DispObj_t* obj, DispObj_t* owner)
{
	if(obj->owner != owner)
		return 0;
	obj->owner = NULL;
	return 1;
}

/* Occupy the object's procedure slot (an ObjProc, see sys/objproc.c);
 * sprites and windows refuse to be deleted while one is attached.  0 when
 * the slot is taken, else 1. */
int DispObj_SetProc(DispObj_t* obj, void* proc)
{
	if(obj->proc)
		return 0;
	obj->proc = proc;
	return 1;
}

// release the procedure slot; 0 when `proc` is not the one attached
int DispObj_ClearProc(DispObj_t* obj, void* proc)
{
	if(obj->proc != proc)
		return 0;
	obj->proc = NULL;
	return 1;
}

// after a child moved on its own, remember its new offset; 1 if `child` was found
int DispObj_UpdateChildOffset(OpenBGI_t* o, DispObj_t* obj, DispObj_t* child)
{
	ChildNode_t* n;
	for(n = obj->children; n; n = n->next)
	{
		if(n->obj == child)
		{
			int32_t po[2], pc[2];
			obj->vt->getPos(o, obj, po);
			child->vt->getPos(o, child, pc);
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

// "90 31"; passed on to the children when setParam 0xC0 asked for it (1.573 on)
void DispObj_SetEnabled(DispObj_t* obj, int f)
{
	ChildNode_t* n;
	obj->enabled = f;
	if(obj->enabledToChildren)
		for(n = obj->children; n; n = n->next)
			DispObj_SetEnabled(n->obj, f);
}

// "91 31" (1.573 on); passed on to the children when setParam 0xC1 asked for it
void DispObj_SetHidden(DispObj_t* obj, int v)
{
	ChildNode_t* n;
	obj->hidden = v;
	if(obj->hiddenToChildren)
		for(n = obj->children; n; n = n->next)
			DispObj_SetHidden(n->obj, v);
}

// the caller re-sorts the object in the compositor afterwards ("90 3A")
void DispObj_SetPriority(DispObj_t* obj, uint32_t p)
{
	obj->priority = (int32_t)p;
}

uint32_t DispObj_GetPriority(DispObj_t* obj)
{
	return (uint32_t)obj->priority;
}

void DispObj_SetEffect(DispObj_t* obj, int e)
{
	obj->effect = e;
}

int DispObj_GetEffect(DispObj_t* obj)
{
	return obj->effect;
}

// 1: sort by creation order, 0: by depth (the projected sprite switches to 0)
void DispObj_SetFlat(DispObj_t* obj, int f)
{
	obj->flat = f;
}

/* setParam 0x8000: round the fixed position to whole pixels, see
 * DispObj_SetFixedPos.  1.492 on keep a second value, the mode (1: snap at
 * any depth, 0: only at depth 0). */
void DispObj_SetSnap(DispObj_t* obj, int f, int mode)
{
	obj->snapToPixel = f;
	obj->snapMode = mode;
}

int DispObj_GetLevel(OpenBGI_t* o, DispObj_t* obj)
{
	return obj->level;
}

void DispObj_GetPos(OpenBGI_t* o, DispObj_t* obj, int32_t out[2])
{
	out[0] = obj->x;
	out[1] = obj->y;
}

void DispObj_GetOffset(DispObj_t* obj, int32_t out[2])
{
	out[0] = obj->offX;
	out[1] = obj->offY;
}

// "90 37": the draw offset, added to the position at draw time
void DispObj_SetOffset(OpenBGI_t* o, DispObj_t* obj, int dx, int dy)
{
	obj->offX = dx;
	obj->offY = dy;
}

int DispObj_HasPixels(DispObj_t* obj)
{
	return obj->bmp.pixels != NULL;
}

void DispObj_CopyBmp(DispObj_t* obj, Bmp_t* out)
{
	*out = obj->bmp;
}

// the base class has no cache: 0x80000001 (unsupported)
int DispObj_BuildCache(OpenBGI_t* o, DispObj_t* obj)
{
	return (int)0x80000001;
}

// the base draw and notify methods do nothing
void DispObj_NopDraw(OpenBGI_t* o, DispObj_t* obj, Bmp_t* dst, const Rect_t* local, uint32_t minKey)
{
}
void DispObj_NopNotify(OpenBGI_t* o, DispObj_t* obj, uint32_t what, int a, int b)
{
}

void DispObj_GetFixedPos(OpenBGI_t* o, DispObj_t* obj, int32_t out[4])
{
	out[0] = obj->fx;
	out[1] = obj->fy;
	out[2] = obj->fz;
	out[3] = obj->fw;
}

/* "90 34": the fade (0..0x100, an extra transparency), passed on to every
 * child.  1.640 on skip the object itself when bit 0 of its flags (setParam
 * 0xFFFF) is clear; the children are still visited. */
void DispObj_SetFade(OpenBGI_t* o, DispObj_t* obj, int v)
{
	ChildNode_t* n;
	if(o->engine.profile->gen < GEN_1_640 || (obj->flagBits & 1))
		obj->fade = v;
	for(n = obj->children; n; n = n->next)
		DispObj_SetFade(o, n->obj, v);
}

/* The animation progress, passed on to every child.  `raw` 1 stores `v` as
 * it is (8.16); otherwise `v` is an integer step 0..0x100 ("90 35") and is
 * shifted into the fraction. */
void DispObj_SetProgress(OpenBGI_t* o, DispObj_t* obj, int raw, int v)
{
	ChildNode_t* n;
	obj->progress = raw == 1 ? v : v << 16;
	for(n = obj->children; n; n = n->next)
		n->obj->vt->setProgress(o, n->obj, raw, v);
}

// setProgress(0, v): an integer step through the virtual method
void DispObj_SetProgressInt(OpenBGI_t* o, DispObj_t* obj, int v)
{
	obj->vt->setProgress(o, obj, 0, v);
}

// `raw` 1: the 8.16 value; else the integer step (the high 16 bits)
int DispObj_GetProgress(DispObj_t* obj, int raw)
{
	if(raw == 1)
		return obj->progress;
	return (int)(((uint32_t)obj->progress >> 16) & 0xffffu);
}

int DispObj_GetProgressInt(DispObj_t* obj)
{
	return DispObj_GetProgress(obj, 0);
}

/* The level the blitters get: the object's own level combined with the
 * inherited fade according to the effect family.  Effects 0x01 and
 * 0x20..0x24 are transparencies (the opacities multiply), 0x02..0x04, 0xC0
 * and 0xC1 are strengths (the strength shrinks as the object fades);
 * everything else keeps the level unchanged. */
int DispObj_EffectiveLevel(DispObj_t* obj)
{
	uint32_t e = (uint32_t)obj->effect;
	int cls = 2;
	if(e == 0x01 || (e >= 0x20 && e <= 0x24))
		cls = 0;
	else if((e >= 0x02 && e <= 0x04) || e == 0xc0 || e == 0xc1)
		cls = 1;
	// 1.599 on multiplies the opacity of "90 39" in as well
	switch(cls)
	{
		case 0: return 0x100 - (((0x100 - obj->fade) * (0x100 - obj->level) * obj->opacity) >> 16);
		case 1: return ((0x100 - obj->fade) * obj->level * obj->opacity) >> 16;
		default: return obj->level;
	}
}

// "90 39" (1.599 on): the opacity, 0..0x100, passed on to every child
void DispObj_SetOpacity(DispObj_t* obj, int v)
{
	ChildNode_t* n;
	obj->opacity = v;
	for(n = obj->children; n; n = n->next)
		DispObj_SetOpacity(n->obj, v);
}

// ========================================================================
// virtual methods
// ========================================================================

// "90 30" and the class-specific show instructions; passed on to every child
void DispObj_SetVisible(OpenBGI_t* o, DispObj_t* obj, int on)
{
	ChildNode_t* n;
	obj->visible = on;
	for(n = obj->children; n; n = n->next)
		n->obj->vt->setVisible(o, n->obj, on);
}

/* shown, enabled, not hidden (1.573 on), not faded out completely and
 * (1.599 on) not fully transparent */
int DispObj_IsVisible(OpenBGI_t* o, DispObj_t* obj)
{
	return obj->visible && obj->enabled && !obj->hidden && (uint32_t)obj->fade < 0x100u && obj->opacity > 0;
}

/* Hand the object's screen rectangle and sort key to the compositor as a
 * dirty rectangle, then the children's.  An object without a size
 * (bmp.pitch 0) dirties nothing. */
void DispObj_Invalidate(OpenBGI_t* o, DispObj_t* obj)
{
	ChildNode_t* n;
	if(obj->bmp.pitch)
	{
		Rect_t r;
		obj->vt->screenRect(o, obj, &r);
		Gfx_AddDirty(o->gfx.dispGfx, obj->vt->sortKey(o, obj), &r);
	}
	for(n = obj->children; n; n = n->next)
		n->obj->vt->invalidate(o, n->obj);
}

/* The key the compositor orders the objects by, ascending: the priority,
 * then the class order, then the creation order (flat objects) or the depth
 * (farther away sorts lower).  1.69/472 adds the bias of setParam 0x8100.
 * 1.636 adds sort mode 1 (setParam 0x8101): the priority, then the depth
 * over its whole 16-bit range, no class order; from 1.640 on the bias of
 * setParam 0x8100 is added to that key too. */
uint32_t DispObj_SortKey(OpenBGI_t* o, DispObj_t* obj)
{
	uint32_t hi;
	if(obj->sortMode == 1 && o->engine.profile->gen >= GEN_1_636)
	{
		int32_t bias = o->engine.profile->gen >= GEN_1_640 ? obj->sortBias : 0;
		return ((uint32_t)obj->priority << 16) + (uint32_t)(bias - (obj->fz >> 16) + 0x7fff);
	}
	hi = ((uint32_t)(obj->priority * 16 + obj->classOrder) << 16) + (uint32_t)obj->sortBias;
	if(obj->flat)
		return hi + (uint32_t)obj->slotId;
	// depth order: a larger z (farther away) sorts lower
	return hi + ((0xffff8000u - (uint32_t)(uint16_t)(obj->fz >> 16)) & 0xffffu);
}

// the object's rectangle in its own coordinates: {0, 0, w - 1, h - 1}
void DispObj_LocalRect(OpenBGI_t* o, DispObj_t* obj, Rect_t* out)
{
	out->l = 0;
	out->t = 0;
	out->r = obj->bmp.w - 1;
	out->b = obj->bmp.h - 1;
}

// the local rectangle moved to the draw position
void DispObj_ScreenRect(OpenBGI_t* o, DispObj_t* obj, Rect_t* out)
{
	int32_t p[2];
	obj->vt->localRect(o, obj, out);
	obj->vt->getDrawPos(o, obj, p);
	Rect_Offset(out, p[0], p[1]);
}

/* Store the position and move the children with it.  With `tellOwner` the
 * owner re-reads this object's offset from the new position (the object
 * moved on its own, as with "90 33"); children moved by their owner get 0. */
void DispObj_SetPosEx(OpenBGI_t* o, DispObj_t* obj, int x, int y, int tellOwner)
{
	ChildNode_t* n;
	obj->x = x;
	obj->y = y;
	if(tellOwner && obj->owner)
		DispObj_UpdateChildOffset(o, obj->owner, obj);
	for(n = obj->children; n; n = n->next)
		n->obj->vt->setPosEx(o, n->obj, x + n->dx, y + n->dy, 0);
}

// "90 33": setPosEx with the owner told
void DispObj_SetPos(OpenBGI_t* o, DispObj_t* obj, int x, int y)
{
	obj->vt->setPosEx(o, obj, x, y, 1);
}

/* 1.616 on: writes the "91 06" offset to `out` and returns 1 when the object
 * follows it (setParam 0xC4, the default); 0 otherwise, `out` untouched. */
int DispObj_GlobalOffset(OpenBGI_t* o, DispObj_t* obj, int32_t out[2])
{
	if(o->engine.profile->gen < GEN_1_616 || !obj->followOffset)
		return 0;
	out[0] = o->gfx.dispGlobalOffset[0];
	out[1] = o->gfx.dispGlobalOffset[1];
	return 1;
}

/* Where the object is drawn: the position plus the "90 37" offset; 1.492 adds
 * the second offset of "90 36", 1.616 the global one of "91 06". */
void DispObj_GetDrawPos(OpenBGI_t* o, DispObj_t* obj, int32_t out[2])
{
	int32_t off[2];
	DispObj_GetPos(o, obj, out);
	DispObj_GetOffset(obj, off);
	out[0] += off[0] + obj->off2X;
	out[1] += off[1] + obj->off2Y;
	if(DispObj_GlobalOffset(o, obj, off))
	{
		out[0] += off[0];
		out[1] += off[1];
	}
}

// "90 36" (1.492 on): the second draw offset, passed on to every child
void DispObj_SetOffset2(DispObj_t* obj, int dx, int dy)
{
	ChildNode_t* n;
	obj->off2X = dx;
	obj->off2Y = dy;
	for(n = obj->children; n; n = n->next)
		DispObj_SetOffset2(n->obj, dx, dy);
}

/* "91 33": the 16.16 position and depth.  With snapToPixel set, x and y are
 * rounded to whole pixels first - only at depth 0 unless the snap mode is
 * 1 (1.492 on).  A flat object also takes the integer position, as if
 * moved by "90 33". */
void DispObj_SetFixedPos(OpenBGI_t* o, DispObj_t* obj, int32_t fx, int32_t fy, int32_t fz)
{
	if(obj->snapToPixel && (fz == 0 || obj->snapMode == 1)) // (mode 1: 1.492 on)
	{                                                       // round to whole pixels
		fx = (int32_t)(((uint32_t)fx + 0x8000u) & 0xffff0000u);
		fy = (int32_t)(((uint32_t)fy + 0x8000u) & 0xffff0000u);
	}
	obj->fz = fz;
	obj->fx = fx;
	obj->fy = fy;
	if(obj->flat)
		obj->vt->setPosEx(o, obj, fx >> 16, fy >> 16, 1);
}

// "90 32": the level, 0..0x100, passed on to every child
void DispObj_SetLevel(OpenBGI_t* o, DispObj_t* obj, int level)
{
	ChildNode_t* n;
	obj->level = level;
	for(n = obj->children; n; n = n->next)
		n->obj->vt->setLevel(o, n->obj, level);
}

/* "90 38": set parameter `no` to (a, b).  The base class handles
 *   0           the position (a, b)
 *   1           the effect
 *   2           the level
 *   0x8000      snap to pixel; 1.492 on takes a second value, the snap mode
 *   0x8001      1.492 on: no perspective scaling of a projected sprite
 *   0x8100      1.69/472 on: the sort bias
 *   0x8101      1.636 on: the sort mode, 0 or 1
 *   0xC0        1.573 on: pass "90 31" on to the children (enabledToChildren)
 *   0xC1        1.573 on: pass "91 31" on to the children (hiddenToChildren)
 *   0xC4        1.616 on: follow the "91 06" offset (default)
 *   0xC5        1.640 on: a switch, default 1 (stored; its use was not found)
 *   0xFFFF      1.640 on: a = the bits (0: all), b = set or clear them
 *   0x7FFF0000  1.492 on: redraw every frame
 *   0x7FFFFFFF  1.492 on: the 16 script values (a: slot, b: value)
 * Returns 0, 0xffff0001 for a number the build does not support, 0xffff0002
 * for a value out of range.  Subclasses handle their own numbers first and
 * fall back to this. */
int DispObj_SetParam(OpenBGI_t* o, DispObj_t* obj, int no, int a, int b)
{
	switch(no)
	{
		case 0: obj->vt->setPos(o, obj, a, b); return 0;
		case 1: DispObj_SetEffect(obj, a); return 0;
		case 2: obj->vt->setLevel(o, obj, a); return 0;
		case 0xc0:
			if(o->engine.profile->gen < GEN_1_573)
				break;
			obj->enabledToChildren = a;
			return 0;
		case 0xc1:
			if(o->engine.profile->gen < GEN_1_573)
				break;
			obj->hiddenToChildren = a;
			return 0;
		case 0xc4:
			if(o->engine.profile->gen < GEN_1_616)
				break;
			obj->followOffset = a;
			return 0;
		case 0xc5:
			if(o->engine.profile->gen < GEN_1_640)
				break;
			obj->paramC5 = a;
			return 0;
		case 0x8000: DispObj_SetSnap(obj, a, o->engine.profile->gen >= GEN_1_492 ? b : 0); return 0;
		case 0x8001:
			if(o->engine.profile->gen < GEN_1_492)
				break;
			obj->noPerspective = a;
			return 0;
		case 0x8100: // 1.69/472 on
			if(o->engine.profile->gen < GEN_1_69_472)
				break;
			obj->sortBias = a;
			return 0;
		case 0x8101:
			if(o->engine.profile->gen < GEN_1_636)
				break;
			if((uint32_t)a > 1)
				return (int)0xffff0002;
			obj->sortMode = a;
			return 0;
		case 0xffff:
		{
			uint32_t mask = a ? (uint32_t)a : 0xffffffffu;
			if(o->engine.profile->gen < GEN_1_640)
				break;
			obj->flagBits = b ? (obj->flagBits | mask) : (obj->flagBits & ~mask);
			return 0;
		}
		case 0x7fff0000: // 1.492 on
			if(o->engine.profile->gen < GEN_1_492)
				break;
			obj->redrawEveryFrame = a;
			return 0;
		case 0x7fffffff:
			if(o->engine.profile->gen < GEN_1_492)
				break;
			if((uint32_t)a >= 16)
				return (int)0xffff0002;
			obj->userValues[a] = b;
			return 0;
		default: break;
	}
	return (int)0xffff0001; // unsupported parameter
}

// the fixed position with the two offsets of "91 37" / "92 37" added (1.69/472 on)
void DispObj_EffFixedPos(OpenBGI_t* o, DispObj_t* obj, int32_t out[4])
{
	int i;
	obj->vt->getFixedPos(o, obj, out);
	for(i = 0; i < 3; i++)
		out[i] += obj->fixOff1[i] + obj->fixOff2[i];
}

// "91 37" (which 1) / "92 37" or "91 36" (which 2): a 16.16 offset added to the fixed position
void DispObj_SetFixedOffset(DispObj_t* obj, int which, int32_t x, int32_t y, int32_t z)
{
	int32_t* v = which == 2 ? obj->fixOff2 : obj->fixOff1;
	v[0] = x;
	v[1] = y;
	v[2] = z;
}

/* "91 38" (1.69/472 on): read parameter `no` into `out`:
 *   0           the position (two values)
 *   1           the effect
 *   2           the level
 *   3           1.492 on: the priority
 *   0x20        the fixed position (three values)
 *   -1          1.535 on: the value the filter object stores (param_1)
 *   -2          1.616 on: the sort key
 *   0x7FFFFFFF  1.492 on: a script value; out[0] names the slot (0..15)
 *               on entry and receives the value
 * Returns 0, 0xffff0001 for a number the build does not support, 0xffff0002
 * for a slot out of range. */
int DispObj_GetParam(OpenBGI_t* o, DispObj_t* obj, int no, int32_t* out)
{
	int32_t v[4];
	switch(no)
	{
		case 0:
			obj->vt->getPos(o, obj, v);
			out[0] = v[0];
			out[1] = v[1];
			return 0;
		case 1: out[0] = obj->effect; return 0;
		case 2: out[0] = obj->vt->getLevel(o, obj); return 0;
		case 3: // 1.492 on: the priority
			if(o->engine.profile->gen < GEN_1_492)
				break;
			out[0] = obj->priority;
			return 0;
		case -1:
			if(o->engine.profile->gen < GEN_1_535)
				break;
			out[0] = obj->param_1;
			return 0;
		case -2: // the sort key
			if(o->engine.profile->gen < GEN_1_616)
				break;
			out[0] = (int32_t)obj->vt->sortKey(o, obj);
			return 0;
		case 0x20:
			obj->vt->getFixedPos(o, obj, v);
			out[0] = v[0];
			out[1] = v[1];
			out[2] = v[2];
			return 0;
		case 0x7fffffff: // 1.492 on: out[0] names the slot and receives the value
			if(o->engine.profile->gen < GEN_1_492)
				break;
			if((uint32_t)out[0] >= 16)
				return (int)0xffff0002;
			out[0] = obj->userValues[out[0]];
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
void DispObj_BuildHitMask(DispObj_t* obj, const Bmp_t* bmp)
{
	int y, x;
	if(obj->maskBits)
	{
		BGI_Free(obj->maskBits);
		obj->hitEnabled = 0;
		obj->maskW = obj->maskH = obj->maskPitch = 0;
		obj->maskBits = NULL;
	}
	if(bmp)
	{
		obj->maskW = bmp->w;
		obj->maskH = bmp->h;
		obj->maskPitch = (bmp->w + 7) >> 3;
		obj->maskBits = (uint8_t*)BGI_Alloc((size_t)obj->maskPitch * (size_t)obj->maskH + 1);
		for(y = 0; y < obj->maskH; y++)
		{
			const uint8_t* p = bmp->pixels + (size_t)y * bmp->pitch;
			uint8_t* row = obj->maskBits + (size_t)y * obj->maskPitch;
			memset(row, 0, (size_t)obj->maskPitch);
			for(x = 0; x < obj->maskW; x++, p += bmp->bpp)
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
	obj->hitEnabled = 1;
}

// drop the mask and turn hit testing off altogether
void DispObj_DisableHit(DispObj_t* obj)
{
	DispObj_BuildHitMask(obj, NULL);
	obj->hitEnabled = 0;
}

/* "90 3D": 1 when (x, y), in object coordinates, lies inside the object's
 * rectangle and, if there is a mask, on a set bit of it; 0 otherwise or
 * when hit testing is disabled. */
int DispObj_HitTest(OpenBGI_t* o, DispObj_t* obj, int x, int y)
{
	if(!obj->hitEnabled || x < 0 || (uint32_t)x >= (uint32_t)obj->bmp.w || y < 0 || (uint32_t)y >= (uint32_t)obj->bmp.h)
		return 0;
	if(!obj->maskBits)
		return 1;
	if((uint32_t)x >= (uint32_t)obj->maskW || (uint32_t)y >= (uint32_t)obj->maskH)
		return 0;
	return (obj->maskBits[obj->maskPitch * y + (x >> 3)] >> (x & 7)) & 1;
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
int DispObj_AllocLike(OpenBGI_t* o, DispObj_t* obj, Bmp_t* b, PixBuf_t* buf, int w, int h, const Bmp_t* like)
{
	int mode;
	if(like)
		mode = like->mode;
	else
		mode = ScreenMode(o) == PM_RGB32 ? PM_ARGB32 : ScreenMode(o);
	return Bmp_AllocIn(b, buf, w, h, mode);
}

/* Give the object a size without pixels: the descriptor gets w, h, the
 * screen pixel mode and a pitch, which is enough for the rectangle and hit
 * tests.  Returns 0 when a dimension is 0, else 1. */
int DispObj_SetSize(OpenBGI_t* o, DispObj_t* obj, int w, int h)
{
	if(w == 0 || h == 0)
		return 0;
	obj->bmp.w = w;
	obj->bmp.h = h;
	obj->bmp.mode = ScreenMode(o);
	obj->bmp.bpp = ModeBytes(obj->bmp.mode);
	obj->bmp.pitch = obj->bmp.bpp * w;
	obj->bmp.pixels = NULL;
	return 1;
}

// change the recorded size of an object that has no pixels of its own (the virtual object, the wait icon)
void DispObj_SetVirtualSize(DispObj_t* obj, int w, int h)
{
	if(!DispObj_HasPixels(obj))
	{
		obj->bmp.w = w;
		obj->bmp.h = h;
	}
}

// 1 and the size, 0 (outputs untouched) when the object has none
int DispObj_GetSize(DispObj_t* obj, int* w, int* h)
{
	if(obj->bmp.w == 0 || obj->bmp.h == 0)
		return 0;
	*w = obj->bmp.w;
	*h = obj->bmp.h;
	return 1;
}

// move a screen rectangle into object coordinates
void DispObj_ToLocal(OpenBGI_t* o, DispObj_t* obj, Rect_t* r)
{
	int32_t p[2];
	obj->vt->getDrawPos(o, obj, p);
	Rect_Offset(r, -p[0], -p[1]);
}

/* The compositor's way of drawing an object: clip the object's screen
 * rectangle to the screen and to the dirty band, crop the back buffer `g`
 * to that piece and call draw() with the piece in object coordinates (so
 * every draw method may assume dst and `local` have the same size).
 * Returns 1 for an invisible object, otherwise whether the object's screen
 * rectangle lies completely inside `dirty` - the compositor uses that to
 * know when the band holds the whole object. */
int DispObj_DrawClipped(OpenBGI_t* o, DispObj_t* obj, const ScreenBmp_t* g, const Rect_t* dirty, uint32_t minKey)
{
	Rect_t r;
	Bmp_t dst;
	int inside;
	if(!obj->vt->isVisible(o, obj))
		return 1;
	obj->vt->screenRect(o, obj, &r);
	if(!Rect_Clip(&r, &g->screen))
		return 0;
	inside = Rect_Inside(&r, dirty);
	if(Rect_Clip(&r, dirty))
	{
		dst = g->bmp;
		Bmp_Crop(&dst, &r);
		DispObj_ToLocal(o, obj, &r);
		obj->vt->draw(o, obj, &dst, &r, minKey);
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
