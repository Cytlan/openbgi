/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * inputmap.c - the input mapper: what a click reaches (OpenBGI_Targets of
 *              openbgi.h; the owners' side is bgi/inputmap.h)
 *
 * The targets are gathered afresh on each call.  The waits of the threads,
 * the panels, the knobs and the registered sprites report theirs; then the
 * objects the scripts tested the pointer against lately ("90 3D"), the
 * buttons they read themselves ("80 11") and the layers covering
 * everything that nobody claimed - the scripts' own ("80 18") - are
 * added, and the hotspots of the game's settings where the screen has
 * nothing else to click (bgi/inputcfg.h).  The list is ordered by layer key, highest first (the order the
 * engine's hit tests go in; a stable sort keeps the reporters' order
 * among equals), and each target gets its rectangle and the point to
 * click in the picture's pixels: the centre of the rectangle when a click
 * there reaches the target, else the reachable point of a grid over it
 * nearest to the centre.
 *
 * The positions the owners report are the back buffer's, which the
 * pointer's are mapped to (Gfx_MapPoint); the points tried are pixels of
 * the picture mapped the same way, so a click where the list says lands
 * where it was tried.  Nothing here changes the engine's state.
 */
#include <string.h>

#include "bgi/inputmap.h"
#include "bgi/inputcfg.h"
#include "bgi/json.h"
#include "bgi/input.h"
#include "bgi/display.h"
#include "bgi/edit.h"
#include "bgi/inframe.h"
#include "bgi/sys.h"
#include "bgi/vm.h"
#include "bgi/wait.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/gfx/dispobj.h"
#include "bgi/core.h"

#define GRID_MAX   24 // points tried across and down a target whose centre is covered
#define CLAIMS_MAX 64 // claimed layers kept (more than the stacks hold in practice)
#define LAYERS_MAX 64 // unbounded layers looked at

struct TargetSink
{
	TargetDesc_t* d; // the reported targets
	int n, cap;
	uint32_t claims[CLAIMS_MAX];
	int claimCount;
};

// ---- the owners' side --------------------------------------------------------------------------

void TargetDesc_Init(TargetDesc_t* d, int kind, uint32_t layer)
{
	memset(d, 0, sizeof *d);
	d->kind = kind;
	d->layer = layer;
	d->group = d->index = -1;
}

void TargetSink_Add(TargetSink_t* s, const TargetDesc_t* d)
{
	if(s->n == s->cap)
	{
		int cap = s->cap ? s->cap * 2 : 32;
		TargetDesc_t* n = (TargetDesc_t*)BGI_Alloc((size_t)cap * sizeof *n);
		if(s->n)
			memcpy(n, s->d, (size_t)s->n * sizeof *n);
		BGI_Free(s->d);
		s->d = n;
		s->cap = cap;
	}
	s->d[s->n++] = *d;
}

void TargetSink_Claim(TargetSink_t* s, uint32_t key)
{
	if(s->claimCount < CLAIMS_MAX)
		s->claims[s->claimCount++] = key;
}

int Target_ReachLayer(OpenBGI_t* o, const TargetDesc_t* d, int x, int y)
{
	return MouseLayer_HitAt(o, d->layer, x, y);
}

// ---- what the scripts test ---------------------------------------------------------------------

/* Note that a script tested the pointer with `what` (input.h): an entry
 * for the same thing is refreshed, else an empty or the oldest one is
 * taken. */
void Input_NoteProbe(OpenBGI_t* o, int what, uint32_t v)
{
	InputProbe_t* p = o->input.probes;
	uint32_t now = GetTicks(o);
	int i, slot = -1;
	for(i = 0; i < INPUT_PROBES; i++)
	{
		if(p[i].what == what && p[i].v == v)
		{
			p[i].at = now;
			return;
		}
		if(slot < 0 && p[i].what == 0)
			slot = i;
	}
	if(slot < 0)
		for(slot = 0, i = 1; i < INPUT_PROBES; i++)
			if(now - p[i].at > now - p[slot].at)
				slot = i;
	p[slot].what = what;
	p[slot].v = v;
	p[slot].at = now;
}

// whether a script tested `what` within INPUT_PROBE_MS
static int Probed(OpenBGI_t* o, int what, uint32_t v)
{
	const InputProbe_t* p = o->input.probes;
	uint32_t now = GetTicks(o);
	int i;
	for(i = 0; i < INPUT_PROBES; i++)
		if(p[i].what == what && p[i].v == v && now - p[i].at <= INPUT_PROBE_MS)
			return 1;
	return 0;
}

// a click anywhere reaches what reads the buttons directly
static int ReachAnywhere(OpenBGI_t* o, const TargetDesc_t* d, int x, int y)
{
	BGI_UNUSED(o);
	BGI_UNUSED(d);
	BGI_UNUSED(x);
	BGI_UNUSED(y);
	return 1;
}

// a click at (x, y) is on the object the script tests ("90 3D": its own hit test, relative to its position)
static int ReachProbe(OpenBGI_t* o, const TargetDesc_t* d, int x, int y)
{
	DispObj_t* obj = (DispObj_t*)d->ctx;
	int32_t pos[2];
	obj->vt->getPos(o, obj, pos);
	return obj->vt->hitTest(o, obj, x - pos[0], y - pos[1]);
}

// the objects tested lately ("90 3D") and the buttons read directly ("80 11")
static void ProbeTargets(OpenBGI_t* o, TargetSink_t* s)
{
	const InputProbe_t* p = o->input.probes;
	uint32_t now = GetTicks(o);
	int i;
	for(i = 0; i < INPUT_PROBES; i++)
	{
		TargetDesc_t d;
		if(!p[i].what || now - p[i].at > INPUT_PROBE_MS)
			continue;
		if(p[i].what == PROBE_OBJECT)
		{
			DispObj_t* obj = o->gfx.gfx ? Gfx_FindObject(o->gfx.gfx, p[i].v) : NULL;
			int32_t pos[2];
			if(!obj || !obj->vt->isVisible(o, obj))
				continue;
			TargetDesc_Init(&d, OPENBGI_TARGET_PROBE, obj->vt->sortKey(o, obj));
			obj->vt->localRect(o, obj, &d.rect);
			obj->vt->getPos(o, obj, pos);
			d.rect.l += pos[0];
			d.rect.r += pos[0];
			d.rect.t += pos[1];
			d.rect.b += pos[1];
			d.flags = OPENBGI_TARGET_PROBED;
			d.owner = p[i].v;
			d.reach = ReachProbe;
			d.ctx = obj;
			TargetSink_Add(s, &d);
		}
		else if(p[i].what == PROBE_BUTTONS)
		{
			TargetDesc_Init(&d, OPENBGI_TARGET_RAW, 0);
			d.flags = OPENBGI_TARGET_ANYWHERE | OPENBGI_TARGET_PROBED;
			d.whole = 1;
			d.owner = p[i].v;
			d.reach = ReachAnywhere;
			TargetSink_Add(s, &d);
		}
	}
}

/* the layers covering everything that no reporter claimed (each claim
 * accounts for one layer of its key) are the scripts' ("80 18"): one
 * target per key, but the base layer's (1) */
static void LayerTargets(OpenBGI_t* o, TargetSink_t* s)
{
	uint32_t keys[LAYERS_MAX], last = 0;
	int used[CLAIMS_MAX];
	int n = MouseLayer_Unbounded(o, keys, LAYERS_MAX), i, j, have = 0;
	memset(used, 0, sizeof used);
	if(n > LAYERS_MAX)
		n = LAYERS_MAX;
	for(i = 0; i < n; i++)
	{
		TargetDesc_t d;
		for(j = 0; j < s->claimCount; j++)
			if(!used[j] && s->claims[j] == keys[i])
				break;
		if(j < s->claimCount)
		{
			used[j] = 1;
			continue;
		}
		if(keys[i] == 1 || (have && keys[i] == last))
			continue;
		have = 1;
		last = keys[i];
		TargetDesc_Init(&d, OPENBGI_TARGET_LAYER, keys[i]);
		d.flags = OPENBGI_TARGET_ANYWHERE | (Probed(o, PROBE_LAYER, keys[i]) ? OPENBGI_TARGET_PROBED : 0);
		d.whole = 1;
		d.owner = keys[i] >> 20;
		d.reach = Target_ReachLayer;
		TargetSink_Add(s, &d);
	}
}

/* a target hit through a mouse layer (what the script reads itself is
 * not - RAW, PROBE, HOTSPOT -, nor the text entry, which lies over the
 * window) */
static int Layered(const TargetDesc_t* d)
{
	return d->kind != OPENBGI_TARGET_RAW && d->kind != OPENBGI_TARGET_PROBE && d->kind != OPENBGI_TARGET_ENTRY &&
		d->kind != OPENBGI_TARGET_HOTSPOT;
}

// ---- the hotspots of the game's settings -------------------------------------------------------

// an ASCII letter as its lower-case one
static int Lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

// the program file `file` is the one a hotspot names: the same name whatever the case, "._bp" there or not on either
static int SameProgram(const char* file, const char* name)
{
	size_t a = strlen(file), b = strlen(name), i;
	if(a > 4 && Lower(file[a - 1]) == 'p' && Lower(file[a - 2]) == 'b' && file[a - 3] == '_' && file[a - 4] == '.')
		a -= 4;
	if(b > 4 && Lower(name[b - 1]) == 'p' && Lower(name[b - 2]) == 'b' && name[b - 3] == '_' && name[b - 4] == '.')
		b -= 4;
	if(a != b)
		return 0;
	for(i = 0; i < a; i++)
		if(Lower((unsigned char)file[i]) != Lower((unsigned char)name[i]))
			return 0;
	return 1;
}

// a thread has the program `name` loaded
static int ProgramLoaded(OpenBGI_t* o, const char* name)
{
	Thread_t* t;
	const ModuleNode_t* m;
	for(t = o->vm.rootThread; t; t = Thread_Next(t))
		for(m = t->modules; m; m = m->next)
			if(m->name && SameProgram(m->name, name))
				return 1;
	return 0;
}

// a click at (x, y) is in the hotspot's rectangle
static int ReachHotspot(OpenBGI_t* o, const TargetDesc_t* d, int x, int y)
{
	BGI_UNUSED(o);
	return x >= d->rect.l && x <= d->rect.r && y >= d->rect.t && y <= d->rect.b;
}

/* the hotspots (bgi/inputcfg.h) are targets while nothing reported so
 * far has a place of its own and takes a click, each only while its
 * program is loaded when it names one */
static void HotspotTargets(OpenBGI_t* o, TargetSink_t* s)
{
	int i;
	for(i = 0; i < s->n; i++)
	{
		const TargetDesc_t* d = &s->d[i];
		if(!(d->flags & (OPENBGI_TARGET_ANYWHERE | OPENBGI_TARGET_DISABLED)) && (o->input.inputEnable || !Layered(d)))
			return;
	}
	for(i = 0; i < o->input.hotspotCount; i++)
	{
		const InputHotspot_t* h = &o->input.hotspots[i];
		TargetDesc_t d;
		if(h->program[0] && !ProgramLoaded(o, h->program))
			continue;
		TargetDesc_Init(&d, OPENBGI_TARGET_HOTSPOT, 0);
		d.rect.l = h->x;
		d.rect.t = h->y;
		d.rect.r = h->x + h->w - 1;
		d.rect.b = h->y + h->h - 1;
		d.index = i;
		d.reach = ReachHotspot;
		TargetSink_Add(s, &d);
	}
}

// ---- the list ---------------------------------------------------------------------------------

// a back-buffer point in the picture's pixels, and back (Gfx_MapPoint)
static void ToPicture(OpenBGI_t* o, int x, int y, int out[2])
{
	int32_t p[2] = {x, y};
	if(o->gfx.gfx)
		Gfx_MapPoint(o->gfx.gfx, p, x, y, 0);
	out[0] = p[0];
	out[1] = p[1];
}

static void ToBuffer(OpenBGI_t* o, int x, int y, int out[2])
{
	int32_t p[2] = {x, y};
	if(o->gfx.gfx)
		Gfx_MapPoint(o->gfx.gfx, p, x, y, 1);
	out[0] = p[0];
	out[1] = p[1];
}

/* a back-buffer rectangle (inclusive) in the picture's pixels, cut to the
 * picture of w x h; 0 when nothing of it is in the picture */
static int PictureRect(OpenBGI_t* o, const Rect_t* r, int w, int h, OpenBGI_Rect_t* out)
{
	int a[2], b[2];
	ToPicture(o, r->l, r->t, a);
	ToPicture(o, r->r, r->b, b);
	if(a[0] < 0)
		a[0] = 0;
	if(a[1] < 0)
		a[1] = 0;
	if(b[0] > w - 1)
		b[0] = w - 1;
	if(b[1] > h - 1)
		b[1] = h - 1;
	if(a[0] > b[0] || a[1] > b[1])
		return 0;
	out->x = a[0];
	out->y = a[1];
	out->w = b[0] - a[0] + 1;
	out->h = b[1] - a[1] + 1;
	return 1;
}

/* the target's rectangle in the picture of w x h; 0 when nothing of it
 * is in the picture */
static int TargetRect(OpenBGI_t* o, const TargetDesc_t* d, int w, int h, OpenBGI_Rect_t* out)
{
	if(d->whole)
	{
		out->x = out->y = 0;
		out->w = w;
		out->h = h;
		return 1;
	}
	if(d->kind == OPENBGI_TARGET_ENTRY)
	{ // (the entry is laid out in the picture's pixels)
		int x0, y0, x1, y1;
		if(!Edit_Rect(o, out))
			return 0;
		x0 = out->x < 0 ? 0 : out->x;
		y0 = out->y < 0 ? 0 : out->y;
		x1 = out->x + out->w > w ? w : out->x + out->w;
		y1 = out->y + out->h > h ? h : out->y + out->h;
		if(x0 >= x1 || y0 >= y1)
			return 0;
		out->x = x0;
		out->y = y0;
		out->w = x1 - x0;
		out->h = y1 - y0;
		return 1;
	}
	return PictureRect(o, &d->rect, w, h, out);
}

/* would a click at the picture's pixel (x, y) reach the target?  Not
 * where the on-screen keyboard or a window the engine draws in the frame
 * lies: the click is theirs, and one on the text entry reaches only the
 * entry */
static int ReachPixel(OpenBGI_t* o, const TargetDesc_t* d, int x, int y)
{
	int b[2];
	if(Osk_Over(o, x, y) || Child_Over(o, x, y))
		return 0;
	if(d->kind == OPENBGI_TARGET_ENTRY)
		return Edit_Over(o, x, y);
	if(!d->reach || Edit_Over(o, x, y))
		return 0;
	ToBuffer(o, x, y, b);
	return d->reach(o, d, b[0], b[1]);
}

/* where to click: the centre of r when it reaches the target, else the
 * point of a grid of up to GRID_MAX x GRID_MAX cells' centres nearest to
 * it; 0 when none reaches it */
static int ClickPoint(OpenBGI_t* o, const TargetDesc_t* d, const OpenBGI_Rect_t* r, int* cx, int* cy)
{
	int mx = r->x + r->w / 2, my = r->y + r->h / 2;
	int nx = r->w < GRID_MAX ? r->w : GRID_MAX, ny = r->h < GRID_MAX ? r->h : GRID_MAX;
	int i, j, found = 0;
	long best = 0;
	if(ReachPixel(o, d, mx, my))
	{
		*cx = mx;
		*cy = my;
		return 1;
	}
	for(j = 0; j < ny; j++)
	{
		int y = r->y + (int)(((long)(2 * j + 1) * r->h) / (2 * ny));
		for(i = 0; i < nx; i++)
		{
			int x = r->x + (int)(((long)(2 * i + 1) * r->w) / (2 * nx));
			long dist = (long)(x - mx) * (x - mx) + (long)(y - my) * (y - my);
			if(found && dist >= best)
				continue;
			if(!ReachPixel(o, d, x, y))
				continue;
			found = 1;
			best = dist;
			*cx = x;
			*cy = y;
		}
	}
	return found;
}

/* fill `out` from the report; 0 when the target lies outside the picture.
 * `top` is the key of the highest layer that covers everything: a target
 * hit through a lower layer is covered all over (no point is tried). */
static int Finish(OpenBGI_t* o, const TargetDesc_t* d, int w, int h, const int pointer[2], uint32_t top, OpenBGI_Target_t* out)
{
	int layered = Layered(d), px = o->input.pointerX, py = o->input.pointerY;
	memset(out, 0, sizeof *out);
	if(!TargetRect(o, d, w, h, &out->rect))
		return 0;
	out->kind = d->kind;
	out->flags = d->flags;
	if((layered && d->layer < top) || !ClickPoint(o, d, &out->rect, &out->clickX, &out->clickY))
	{
		out->clickX = out->clickY = -1;
		out->flags |= OPENBGI_TARGET_COVERED;
	}
	if(Osk_Over(o, px, py) || Child_Over(o, px, py))
	{ // (the pointer is on something of the frame's)
	}
	else if(d->kind == OPENBGI_TARGET_ENTRY ? Edit_Over(o, px, py) : d->reach && !Edit_Over(o, px, py) && d->reach(o, d, pointer[0], pointer[1]))
		out->flags |= OPENBGI_TARGET_HOVER;
	if(!o->input.inputEnable && layered)
		out->flags |= OPENBGI_TARGET_DISABLED; // ("80 10": the layers report nothing)
	out->drag = out->rect;
	if(d->hasDrag && !PictureRect(o, &d->drag, w, h, &out->drag))
		out->drag = out->rect;
	out->owner = d->owner;
	out->group = d->group;
	out->index = d->index;
	out->layer = d->layer;
	return 1;
}

/* every target reported, the highest layer first (among equals in the
 * order of the reports) into *s, `top` the key of the highest layer that
 * covers everything; 0 (and nothing to free) before the window shows a
 * picture */
static int Gather(OpenBGI_t* o, TargetSink_t* s, uint32_t* top)
{
	Thread_t* t;
	OpenBGI_Rect_t entry;
	int i, j;
	memset(s, 0, sizeof *s);
	*top = 0;
	if(!o->display.windowAlive || !o->screen.pic.pixels || o->screen.pic.w <= 0 || o->screen.pic.h <= 0)
		return 0;
	if(Inframe_Up(o))
		return 0; // a dialog drawn in the frame takes every click
	i = Edit_Rect(o, &entry);
	if(i)
	{ // the text entry the engine draws: over everything of the window's
		TargetDesc_t d;
		TargetDesc_Init(&d, OPENBGI_TARGET_ENTRY, 0xffffffffu);
		d.flags = i == 2 ? OPENBGI_TARGET_SELECTED : 0;
		TargetSink_Add(s, &d);
	}
	for(t = o->vm.rootThread ? Thread_Next(o->vm.rootThread) : NULL; t; t = Thread_Next(t))
		if((t->flags & THR_FLAG_WAITING) && t->wait)
			Wait_Targets(o, t->wait, s);
	Panel_TargetsAll(o, s);
	Knob_Targets(o, s);
	Target_Targets(o, s);
	ProbeTargets(o, s);
	LayerTargets(o, s);
	HotspotTargets(o, s);
	for(i = 1; i < s->n; i++)
	{
		TargetDesc_t d = s->d[i];
		for(j = i; j > 0 && s->d[j - 1].layer < d.layer; j--)
			s->d[j] = s->d[j - 1];
		s->d[j] = d;
	}
	MouseLayer_Unbounded(o, top, 1);
	return 1;
}

int InputMap_Targets(OpenBGI_t* o, OpenBGI_Target_t* out, int max)
{
	TargetSink_t s;
	uint32_t top;
	int pointer[2], i, n = 0;
	if(!Gather(o, &s, &top))
		return 0;
	GetMouseClientPos(o, pointer);
	for(i = 0; i < s.n; i++)
	{
		OpenBGI_Target_t one;
		if(!Finish(o, &s.d[i], o->screen.pic.w, o->screen.pic.h, pointer, top, &one))
			continue;
		if(n < max && out)
			out[n] = one;
		n++;
	}
	BGI_Free(s.d);
	return n;
}

int OpenBGI_Targets(OpenBGI_t* o, OpenBGI_Target_t* out, int max, size_t size)
{
	OpenBGI_Target_t* all;
	int n, i;
	if(size == sizeof *out || !out || max <= 0)
		return InputMap_Targets(o, out, max);
	all = (OpenBGI_Target_t*)BGI_Calloc((size_t)max * sizeof *all); // the host's are of another size: what its own hold of each
	n = InputMap_Targets(o, all, max);
	for(i = 0; i < n && i < max; i++)
	{
		char* to = (char*)out + (size_t)i * size;
		memset(to, 0, size);
		memcpy(to, &all[i], size < sizeof *all ? size : sizeof *all);
	}
	BGI_Free(all);
	return n;
}

// ---- where a touch goes (src/sys/touch.c) ---------------------------------------------------------

// how far the picture's pixel (x, y) lies from the rectangle (0 inside), squared
static long DistanceSq(const OpenBGI_Rect_t* r, int x, int y)
{
	long dx = 0, dy = 0;
	if(x < r->x)
		dx = r->x - x;
	else if(x >= r->x + r->w)
		dx = x - (r->x + r->w - 1);
	if(y < r->y)
		dy = r->y - y;
	else if(y >= r->y + r->h)
		dy = y - (r->y + r->h - 1);
	return dx * dx + dy * dy;
}

/* The target nearest to the picture's pixel (x, y) within `radius`
 * pixels that is not one a click anywhere reaches, takes a click now and
 * has a point that does (only knobs with `knobs`): its index in s, the
 * point in out[]; -1 for none. */
static int Nearest(OpenBGI_t* o, const TargetSink_t* s, uint32_t top, int x, int y, int radius, int knobs, int out[2])
{
	int i, best = -1;
	long bestD = 0;
	for(i = 0; i < s->n; i++)
	{
		const TargetDesc_t* d = &s->d[i];
		OpenBGI_Rect_t r;
		long dist;
		int c[2];
		if((d->flags & (OPENBGI_TARGET_ANYWHERE | OPENBGI_TARGET_DISABLED)) || d->whole || (knobs && d->kind != OPENBGI_TARGET_KNOB))
			continue;
		if((Layered(d) && (d->layer < top || !o->input.inputEnable)) || !TargetRect(o, d, o->screen.pic.w, o->screen.pic.h, &r))
			continue;
		dist = DistanceSq(&r, x, y);
		if(dist > (long)radius * radius || (best >= 0 && dist >= bestD) || !ClickPoint(o, d, &r, &c[0], &c[1]))
			continue;
		best = i;
		bestD = dist;
		out[0] = c[0];
		out[1] = c[1];
	}
	return best;
}

int InputMap_TapPoint(OpenBGI_t* o, int x, int y, int radius, int out[2], int* kind, uint32_t* owner)
{
	TargetSink_t s;
	uint32_t top;
	int i, hit = -1;
	out[0] = x;
	out[1] = y;
	*kind = 0;
	*owner = 0;
	if(Osk_Over(o, x, y) || Child_Over(o, x, y))
		return 0; // on something of the frame's: the tap is for that, where it is
	if(!Gather(o, &s, &top))
		return 0;
	for(i = 0; i < s.n && hit < 0; i++) // what is under the finger, a target a click anywhere reaches last
		if(!(s.d[i].flags & OPENBGI_TARGET_ANYWHERE) && !(Layered(&s.d[i]) && s.d[i].layer < top) && ReachPixel(o, &s.d[i], x, y))
			hit = i;
	if(hit < 0)
		hit = Nearest(o, &s, top, x, y, radius, 0, out);
	for(i = 0; i < s.n && hit < 0; i++)
		if(!(Layered(&s.d[i]) && s.d[i].layer < top) && ReachPixel(o, &s.d[i], x, y))
			hit = i;
	if(hit >= 0)
	{
		*kind = s.d[hit].kind;
		*owner = s.d[hit].owner;
	}
	BGI_Free(s.d);
	if(hit < 0)
		return 0;
	return out[0] == x && out[1] == y ? 1 : 2;
}

int InputMap_KnobPoint(OpenBGI_t* o, int x, int y, int radius, int out[2])
{
	TargetSink_t s;
	uint32_t top;
	int i, under = 0, found = 0;
	if(!Gather(o, &s, &top))
		return 0;
	for(i = 0; i < s.n && !under; i++) // what is under the finger (what the script tests the pointer against aside)
	{
		const TargetDesc_t* d = &s.d[i];
		if((d->flags & OPENBGI_TARGET_ANYWHERE) || d->kind == OPENBGI_TARGET_PROBE || (Layered(d) && d->layer < top) || !ReachPixel(o, d, x, y))
			continue;
		under = 1;
		if(d->kind == OPENBGI_TARGET_KNOB && o->input.inputEnable && !(d->flags & OPENBGI_TARGET_DISABLED))
		{
			out[0] = x; // on the thumb: hold it where the finger is
			out[1] = y;
			found = 1;
		}
	}
	if(!under)
		found = Nearest(o, &s, top, x, y, radius, 1, out) >= 0;
	BGI_Free(s.d);
	return found;
}

// ---- where a pad goes (src/sys/pad.c) ---------------------------------------------------------

#define LIST_MAX 256 // targets looked at

int InputMap_Focusables(OpenBGI_t* o, OpenBGI_Target_t* out, int max)
{
	OpenBGI_Target_t all[LIST_MAX];
	int n = InputMap_Targets(o, all, LIST_MAX), i, j, m = 0;
	if(n > LIST_MAX)
		n = LIST_MAX;
	for(i = 0; i < n && m < max; i++)
	{
		const OpenBGI_Target_t* t = &all[i];
		if((t->flags & (OPENBGI_TARGET_ANYWHERE | OPENBGI_TARGET_DISABLED | OPENBGI_TARGET_COVERED)) || t->clickX < 0 ||
			t->kind == OPENBGI_TARGET_PANEL)
			continue;
		for(j = 0; j < m; j++) // a place is one target: the one listed first, which a click reaches first
			if(out[j].rect.x == t->rect.x && out[j].rect.y == t->rect.y && out[j].rect.w == t->rect.w && out[j].rect.h == t->rect.h)
				break;
		if(j == m)
			out[m++] = *t;
	}
	return m;
}

// the picture's pixel (x, y) lies on a target with a place of its own that takes a click now (not all[skip])
static int OnTarget(const OpenBGI_Target_t* all, int n, int skip, int x, int y)
{
	int i;
	for(i = 0; i < n; i++)
	{
		const OpenBGI_Rect_t* r = &all[i].rect;
		if(i != skip && !(all[i].flags & (OPENBGI_TARGET_ANYWHERE | OPENBGI_TARGET_DISABLED | OPENBGI_TARGET_COVERED)) && x >= r->x &&
			y >= r->y && x < r->x + r->w && y < r->y + r->h)
			return 1;
	}
	return 0;
}

#define FREE_GRID 9 // points tried across and down the picture for one that is on no target

int InputMap_AdvancePoint(OpenBGI_t* o, int at[2])
{
	OpenBGI_Target_t all[LIST_MAX];
	int n = InputMap_Targets(o, all, LIST_MAX), i, j, a;
	if(n > LIST_MAX)
		n = LIST_MAX;
	for(a = 0; a < n; a++)
	{
		const OpenBGI_Target_t* t = &all[a];
		if((t->flags & OPENBGI_TARGET_ANYWHERE) && !(t->flags & (OPENBGI_TARGET_DISABLED | OPENBGI_TARGET_COVERED)) && t->clickX >= 0 &&
			(t->kind == OPENBGI_TARGET_TEXT || t->kind == OPENBGI_TARGET_WAIT || t->kind == OPENBGI_TARGET_LAYER || t->kind == OPENBGI_TARGET_RAW))
			break;
	}
	if(a == n)
		return 0;
	at[0] = all[a].clickX;
	at[1] = all[a].clickY;
	if(all[a].flags & OPENBGI_TARGET_HOVER)
	{ // the pointer is on it: unless it is on something else too, a click where it is goes on
		for(i = 0; i < n; i++)
			if(i != a && (all[i].flags & OPENBGI_TARGET_HOVER) && !(all[i].flags & (OPENBGI_TARGET_ANYWHERE | OPENBGI_TARGET_COVERED)))
				break;
		if(i == n)
		{
			at[0] = o->input.pointerX;
			at[1] = o->input.pointerY;
			return 1;
		}
	}
	if(all[a].kind == OPENBGI_TARGET_RAW && OnTarget(all, n, a, at[0], at[1]))
	{ // the script reads the buttons wherever the pointer is: a place where the click is no button's as well
		int w = o->screen.pic.w, h = o->screen.pic.h;
		for(j = 0; j < FREE_GRID; j++)
			for(i = 0; i < FREE_GRID; i++)
			{
				int x = (2 * i + 1) * w / (2 * FREE_GRID), y = (2 * j + 1) * h / (2 * FREE_GRID);
				if(!OnTarget(all, n, a, x, y))
				{
					at[0] = x;
					at[1] = y;
					return 1;
				}
			}
	}
	return 1;
}

// ---- once a frame -----------------------------------------------------------------------------

void InputMap_Stop(OpenBGI_t* o)
{
	Osk_Free(o);
	Pad_Stop(o);
	memset(&o->input.steps, 0, sizeof o->input.steps); // (what they were to press is gone with the run)
	o->input.device = INPUT_DEV_NONE;
	InputMap_Configure(o, NULL, NULL); // the game's settings go with it
}

// one "input" object over what is set so far
static void Settings(OpenBGI_t* o, const InputSettings_t* s)
{
	int i;
	if(!s)
		return;
	for(i = 0; i < PAD_CONTROLS; i++)
		if(s->bound[i])
			o->input.pad.bind[i] = s->bind[i];
	if(s->ownPadKeys >= 0)
		o->input.pad.ownKeysOff = !s->ownPadKeys;
	if(s->tapDelay >= 0)
		o->input.steps.tapDelay = s->tapDelay;
	if(s->pointer >= 0)
		o->input.pad.arrowFor = s->pointer;
	if(s->pointerSize >= 0)
		o->input.pad.arrowSize = s->pointerSize;
	if(s->stickSpeed >= 0)
		o->input.pad.stickSpeed = s->stickSpeed;
	if(s->onScreenKeyboard >= 0)
		o->input.oskAsked = !s->onScreenKeyboard;
	if(s->settingsKey >= 0)
		o->input.settingsKey = s->settingsKey;
	if(s->hotspotCount > 0)
	{ // a list replaces the one before
		InputHotspot_t* copy = (InputHotspot_t*)BGI_Alloc(sizeof *copy * (size_t)s->hotspotCount);
		if(!copy)
			return;
		memcpy(copy, s->hotspots, sizeof *copy * (size_t)s->hotspotCount);
		BGI_Free(o->input.hotspots);
		o->input.hotspots = copy;
		o->input.hotspotCount = s->hotspotCount;
	}
}

void InputMap_Configure(OpenBGI_t* o, const InputSettings_t* all, const InputSettings_t* game)
{
	Pad_Init(o); // the bindings, the pointer and the stick as they are without settings
	o->input.steps.tapDelay = 0;
	o->input.oskAsked = 0;
	o->input.settingsKey = 0x78; // F9
	BGI_Free(o->input.hotspots);
	o->input.hotspots = NULL;
	o->input.hotspotCount = 0;
	o->input.cfgAll = all;
	o->input.cfgGame = game;
	Settings(o, o->input.cfgHostBase);
	Settings(o, all);
	Settings(o, game);
	Settings(o, o->input.cfgHost);
	if(o->engine.optOwnPadKeys) // the command line's --own-pad-keys= comes before the settings
		o->input.pad.ownKeysOff = o->engine.optOwnPadKeys == 2;
}

int OpenBGI_InputSettings(OpenBGI_t* o, const char* json)
{
	InputSettings_t *s = NULL, *base = NULL;
	if(json && json[0])
	{
		JsonNode_t* root = Json_Parse(json, strlen(json), NULL);
		if(!root || root->type != JSON_OBJECT)
		{
			Json_Free(root);
			return 0;
		}
		s = InputSettings_Parse(root, "the host's settings");
		base = InputSettings_Parse(Json_Get(root, "defaults"), "the host's settings, \"defaults\"");
		Json_Free(root);
	}
	InputSettings_Free(o->input.cfgHost);
	InputSettings_Free(o->input.cfgHostBase);
	o->input.cfgHost = s;
	o->input.cfgHostBase = base;
	InputMap_Configure(o, o->input.cfgAll, o->input.cfgGame);
	return 1;
}

void InputMap_Free(OpenBGI_t* o)
{
	InputSettings_Free(o->input.cfgHost);
	InputSettings_Free(o->input.cfgHostBase);
	o->input.cfgHost = o->input.cfgHostBase = NULL;
}

void InputMap_Frame(OpenBGI_t* o)
{
	Touch_Frame(o); // a touch held long enough
	Pad_Frame(o);   // a direction held, the sticks, the focus
	Osk_Frame(o);   // the on-screen keyboard comes or goes
	Steps_Frame(o); // what all of them do to the mouse and the keys, a step a frame
}
