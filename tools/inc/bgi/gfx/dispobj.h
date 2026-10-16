/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dispobj.h - DispObj, the base class of everything the compositor draws:
 * the object layout, its vtable of 24 virtual methods and the base
 * implementations (src/gfx/dispobj.c).
 *
 * The C++ class hierarchy is mapped onto C as follows: every object starts
 * with a DispObj, every class has a static vtable structure whose first
 * member is the DispObjVtbl (subclasses that add virtual methods wrap it),
 * and `o->vt->method(o, ...)` is the virtual call.  Constructors take the
 * already-allocated object; the deleting destructor (slot 0) frees it.
 *
 * An object is placed with an integer position (plus up to three draw
 * offsets) or a 16.16 fixed position with a depth, is ordered by its sort
 * key (priority, class, creation order or depth), is blended with an
 * effect mode and a level, and may own children that follow its position,
 * visibility, level, fade and opacity.  The structure below describes
 * every field, the vtable every method.
 */
#ifndef BGI_GFX_DISPOBJ_H_
#define BGI_GFX_DISPOBJ_H_

#include "bgi/gfx/bmpmgr.h"

typedef struct DispObj DispObj_t;
typedef struct Gfx Gfx_t; // the compositor / object manager (gfxmgr.h)

typedef struct ChildNode // one entry of an object's child list
{
	DispObj_t* obj;
	int32_t dx, dy; // offset of the child from the owner, pixels
	struct ChildNode* next;
} ChildNode_t;

typedef struct DispObjVtbl
{
	void (*destroy)(DispObj_t* o, int flags);                                     // deleting dtor (flags & 1: free the memory)
	void (*setVisible)(DispObj_t* o, int on);                                     // "90 30" and the per-class show instructions
	int (*isVisible)(DispObj_t* o);                                               // shown, enabled, not faded out
	void (*invalidate)(DispObj_t* o);                                             // hand the screen rectangle to the compositor
	void (*draw)(DispObj_t* o, Bmp_t* dst, const Rect_t* local, uint32_t minKey); // paint `local` (object coordinates) into the cropped `dst`
	uint32_t (*sortKey)(DispObj_t* o);                                            // ascending draw order
	void (*localRect)(DispObj_t* o, Rect_t* out);                                 // {0, 0, w - 1, h - 1}
	void (*screenRect)(DispObj_t* o, Rect_t* out);                                // localRect at the draw position
	void (*setPosEx)(DispObj_t* o, int x, int y, int tellOwner);                  // tellOwner: the owner re-reads the child offset
	void (*setPos)(DispObj_t* o, int x, int y);                                   // "90 33": setPosEx(x, y, 1)
	void (*getPos)(DispObj_t* o, int32_t out[2]);
	void (*getDrawPos)(DispObj_t* o, int32_t out[2]);                      // position plus the draw offsets
	void (*setOffset)(DispObj_t* o, int dx, int dy);                       // "90 37"
	void (*setFixedPos)(DispObj_t* o, int32_t fx, int32_t fy, int32_t fz); // "91 33": 16.16 position and depth
	void (*getFixedPos)(DispObj_t* o, int32_t out[4]);                     // fx, fy, fz, fw
	void (*setLevel)(DispObj_t* o, int level);                             // "90 32", with the children
	int (*getLevel)(DispObj_t* o);
	void (*setProgress)(DispObj_t* o, int raw, int v);         // "90 35"; raw 1: v is 8.16, else an integer step
	int (*setParam)(DispObj_t* o, int no, int a, int b);       // "90 38"; 0 ok, 0xffff0001 unknown, 0xffff0002 bad value
	int (*hitTest)(DispObj_t* o, int x, int y);                // "90 3D", object coordinates
	void (*notify)(DispObj_t* o, uint32_t what, int a, int b); // 0xF0000000 after every rendered frame
	int (*buildCache)(DispObj_t* o);                           // "90 3F"; 0x80000001 when the class has none
	int (*setSize)(DispObj_t* o, int w, int h);                // 0 on a zero dimension
	/* 1.69/472 on: read a parameter, see DispObj_GetParam; the sprite adds
	 * its own.  Kept last here so that the slots above stay those of the
	 * reference build. */
	int (*getParam)(DispObj_t* o, int no, int32_t* out);
} DispObjVtbl_t;

struct DispObj
{
	const DispObjVtbl_t* vt;
	int32_t enabled;      // default 1 ("90 31")
	int32_t visible;      // default 0
	int32_t classOrder;   // 0..10, fixed per class (see objects.h)
	int32_t priority;     // 0..0xFFF, the script's "prio"
	int32_t slotId;       // creation counter & 0x7FFF: later objects on top
	int32_t x, y;         // position, pixels
	int32_t offX, offY;   // draw offset ("90 37"), pixels
	int32_t fx, fy;       // 16.16 position
	int32_t fz;           // 16.16 depth
	int32_t fw;           // only copied out by getFixedPos
	int32_t flat;         // 1: order by slotId, 0: by depth
	int32_t snapToPixel;  // setParam 0x8000: round the fixed position
	PixBuf_t* cacheBuf;   // pixel holder, always allocated
	Bmp_t bmp;            // size (and sometimes pixels)
	int32_t effect;       // blend mode, default 0x80 (see bitmap.h)
	int32_t level;        // 0..0x100; transparency or strength depending on the effect
	int32_t fade;         // 0..0x100, extra transparency inherited by children
	int32_t progress;     // 8.16 animation parameter
	int32_t hitEnabled;   // default 1
	int32_t maskW, maskH; // hit mask size, pixels
	int32_t maskPitch;    // (maskW + 7) / 8, bytes
	uint8_t* maskBits;    // 1 bit per pixel; NULL = the whole rectangle hits
	DispObj_t* owner;     // the object this one is attached to
	int32_t pad84[3];     // unused (the first word is the list pseudo head of the original)
	ChildNode_t* children;
	void* proc; // attached script procedure (sys/objproc.c)
	/* the fields the later builds added (DispObj_SetParam / DispObj_GetParam
	 * list the parameter numbers):
	 * 1.69/472 "91 37": a 16.16 offset added to the fixed position, "92 37"
	 * / "91 36": a second one; setParam 0x8100: added to the sort key;
	 * 1.494 setParam 0x7FFF0000: invalidate every frame; setParam / getParam
	 * 0x7FFFFFFF: 16 values the scripts store with the object */
	int32_t fixOff1[3];
	int32_t fixOff2[3];
	int32_t sortBias;
	int32_t redrawEveryFrame;
	int32_t userValues[16];
	int32_t off2X, off2Y; // 1.494: "90 36", a second draw offset (also handed to the children)
	int32_t opacity;      // 1.599: "90 39", a second multiplier on the level, 0..0x100 (default), inherited by the children
	/* the remaining setParam switches of the later builds (DispObj_SetParam) */
	int32_t snapMode;      // 1.494: setParam 0x8000's second value; 1 snaps at any depth
	int32_t noPerspective; // 1.494: setParam 0x8001; a sprite's depth no longer scales it (SpProjDist)
	int32_t followOffset;  // 1.616: setParam 0xC4, default 1; the object moves with the "91 06" offset
	int32_t paramC5;       // 1.640: setParam 0xC5, default 1 (stored; its use was not found)
	int32_t sortMode;      // 1.640: setParam 0x8101; 1 orders by priority and depth alone (DispObj_SortKey)
	uint32_t flagBits;     // 1.640: setParam 0xFFFF, default all set; bit 0 lets "90 34" change this object's fade
	int32_t param_1;       // 1.535: read by getParam -1; the filter object stores a result code there
};

extern int32_t gDispGlobalOffset[2]; // 1.616 on: "91 06", added to the draw position of the objects that follow it

extern const DispObjVtbl_t DispObj_Vtbl;
extern Gfx_t* gDispGfx;        // the graphics manager (set by DispObj_SetGfxPtr)
extern BmpMgr_t* gDispBmpMgr;  // the bitmap manager (set by DispObj_SetBmpMgrPtr)
extern int gGlobalEffectValue; // "90 08": stored only

void DispObj_Ctor(DispObj_t* o, int classOrder, int slotId);
void DispObj_Dtor(DispObj_t* o);               // body only
void DispObj_Destroy(DispObj_t* o, int flags); // dtor (+ free if flags & 1)
void DispObj_SetGfxPtr(Gfx_t* g);
void DispObj_SetBmpMgrPtr(BmpMgr_t* m);
void Gfx_SetGlobalEffectValue(int v);

int32_t Ease(int32_t t, int curve);                       // 8.16 progress -> 16.16 factor; curve 0 linear, 1..15 see dispobj.c
void ProjectScale(int32_t* out, int32_t z, int32_t dist); // 16.16 perspective factor for a 16.16 depth

// tree: 1 ok, 0 refused (see dispobj.c for the conditions)
int DispObj_AttachChild(DispObj_t* o, DispObj_t* child, int dx, int dy);
int DispObj_DetachChild(DispObj_t* o, DispObj_t* child);
int DispObj_SetOwner(DispObj_t* o, DispObj_t* owner);
int DispObj_ClearOwner(DispObj_t* o, DispObj_t* owner);
int DispObj_SetProc(DispObj_t* o, void* proc);
int DispObj_ClearProc(DispObj_t* o, void* proc);
int DispObj_UpdateChildOffset(DispObj_t* o, DispObj_t* child); // 1 if child found

// plain setters / getters
void DispObj_SetEnabled(DispObj_t* o, int f);
void DispObj_SetPriority(DispObj_t* o, uint32_t prio);
uint32_t DispObj_GetPriority(DispObj_t* o);
void DispObj_SetEffect(DispObj_t* o, int effect);
int DispObj_GetEffect(DispObj_t* o);
void DispObj_SetFade(DispObj_t* o, int v); // "90 34", with the children
void DispObj_SetFlat(DispObj_t* o, int f);
void DispObj_SetSnap(DispObj_t* o, int f, int mode);    // setParam 0x8000
int DispObj_GlobalOffset(DispObj_t* o, int32_t out[2]); // 1.616 on: 1 and the "91 06" offset when the object follows it, else 0
void DispObj_SetProgressInt(DispObj_t* o, int v);       // setProgress(0, v)
int DispObj_GetProgress(DispObj_t* o, int raw);         // raw 1: 8.16, else the integer step
int DispObj_GetProgressInt(DispObj_t* o);
void DispObj_GetOffset(DispObj_t* o, int32_t out[2]);
int DispObj_EffectiveLevel(DispObj_t* o);                // level, fade and opacity combined for the blitters
void DispObj_SetVirtualSize(DispObj_t* o, int w, int h); // only when the object has no pixels
int DispObj_GetSize(DispObj_t* o, int* w, int* h);       // 0 when the object has no size
int DispObj_HasPixels(DispObj_t* o);
// 1.69/472 on: the fixed position with the two offsets of "91 37" / "92 37" added
void DispObj_EffFixedPos(DispObj_t* o, int32_t out[4]);
void DispObj_SetOffset2(DispObj_t* o, int dx, int dy);                                 // 1.494 on, "90 36"
void DispObj_SetFixedOffset(DispObj_t* o, int which, int32_t x, int32_t y, int32_t z); // which 1 ("91 37") / 2 ("92 37", "91 36")
/* 1.69/472 on ("91 38"): read a parameter into out
 * (0 position, 1 effect, 2 level, 0x20 fixed position; 1.494 adds 3 the
 * priority and 0x7FFFFFFF a script value, 1.535 -1 the filter's value,
 * 1.616 -2 the sort key); 0 ok, 0xffff0001 unknown, 0xffff0002 bad slot */
int DispObj_GetParam(DispObj_t* o, int no, int32_t* out);
void DispObj_CopyBmp(DispObj_t* o, Bmp_t* out);
void Gfx_GetBackBmp(Bmp_t* out);                                 // gDispGfx's back buffer
void DispObj_ToLocal(DispObj_t* o, Rect_t* r);                   // screen to object coordinates
void DispObj_BuildHitMask(DispObj_t* o, const Bmp_t* bmpOrNull); // "90 3C"; NULL drops the mask
void DispObj_DisableHit(DispObj_t* o);
int Bmp_AllocIn(Bmp_t* b, PixBuf_t* buf, int w, int h, int mode);                                    // 1 when pixels were allocated
int DispObj_AllocLike(DispObj_t* o, Bmp_t* b, PixBuf_t* buf, int w, int h, const Bmp_t* likeOrNull); // mode of `like`, or the screen's with alpha

/* The compositor's entry point: `g` is the back buffer together with the
 * screen rectangle; `dirty` the band to paint.  Returns 1 for an invisible
 * object, else whether the object's screen rectangle lies completely inside
 * `dirty`. */
typedef struct ScreenBmp
{
	Bmp_t bmp;     // the back buffer
	Rect_t screen; // its rectangle, {0, 0, w - 1, h - 1}
} ScreenBmp_t;
int DispObj_DrawClipped(DispObj_t* o, const ScreenBmp_t* g, const Rect_t* dirty, uint32_t minKey);

// the base implementations, for subclass vtables
void DispObj_SetVisible(DispObj_t* o, int on);
int DispObj_IsVisible(DispObj_t* o);
void DispObj_Invalidate(DispObj_t* o);
void DispObj_NopDraw(DispObj_t* o, Bmp_t* dst, const Rect_t* local, uint32_t minKey);
uint32_t DispObj_SortKey(DispObj_t* o);
void DispObj_LocalRect(DispObj_t* o, Rect_t* out);
void DispObj_ScreenRect(DispObj_t* o, Rect_t* out);
void DispObj_SetPosEx(DispObj_t* o, int x, int y, int tellOwner);
void DispObj_SetPos(DispObj_t* o, int x, int y);
void DispObj_GetPos(DispObj_t* o, int32_t out[2]);
void DispObj_GetDrawPos(DispObj_t* o, int32_t out[2]);
void DispObj_SetOffset(DispObj_t* o, int dx, int dy);
void DispObj_SetFixedPos(DispObj_t* o, int32_t fx, int32_t fy, int32_t fz);
void DispObj_GetFixedPos(DispObj_t* o, int32_t out[4]);
void DispObj_SetLevel(DispObj_t* o, int level);
void DispObj_SetOpacity(DispObj_t* o, int v); // 1.599 on ("90 39"): with the children
int DispObj_GetLevel(DispObj_t* o);
void DispObj_SetProgress(DispObj_t* o, int raw, int v);
int DispObj_SetParam(DispObj_t* o, int no, int a, int b);
int DispObj_HitTest(DispObj_t* o, int x, int y);
void DispObj_NopNotify(DispObj_t* o, uint32_t what, int a, int b);
int DispObj_BuildCache(DispObj_t* o); // 0x80000001: unsupported
int DispObj_SetSize(DispObj_t* o, int w, int h);

// the manager hooks the display objects need (gfxmgr.c)
void Gfx_AddDirty(Gfx_t* g, uint32_t key, const Rect_t* r);
void Gfx_InvalidateAll(Gfx_t* g);
void Gfx_CopyBackBmp(Gfx_t* g, Bmp_t* out); // the back buffer

#endif // BGI_GFX_DISPOBJ_H_
