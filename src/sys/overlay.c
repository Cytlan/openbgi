/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * overlay.c - the screen layer and the engine's on-screen controls
 *             (src/include/bgi/overlay.h): where they lie, how they are
 *             drawn, what a finger on them does; not part of the
 *             original engine
 *
 * The layer is a picture of the surface's size, four bytes a pixel (blue,
 * green, red, alpha; the colours not multiplied by the alpha), clear but
 * for the controls.  Each control has a square of its own in it, apart
 * from every other's: a control that changes - a finger on it, the
 * stick's knob moved - has its square cleared and drawn anew, and the
 * frame says which part of the layer changed.
 *
 * The shapes are drawn from their distance to each pixel's centre, which
 * gives smooth edges at any size: a disc, a ring, a line with round
 * ends, a box with round corners, a triangle.
 */
#include "bgi/overlay.h"
#include "bgi/input.h"
#include "bgi/host.h"
#include "bgi/version.h"
#include "bgi/engine.h"
#include "bgi/launcher.h"
#include "bgi/settings.h"
#include "bgi/perf.h"
#include "bgi/display.h"
#include "bgi/core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DPI_UNKNOWN 160 // pixels an inch of a surface that does not say

/* Where the controls lie at their full size, in tenths of a millimetre
 * from the edges of the surface's free part: the D-pad low on the left
 * with the stick over it; on the right a column 16 mm wide - Confirm at
 * the bottom, then Cancel, Skip, Forward and Backlog - with the pad's
 * own button in the corner over it; the settings button in the top left
 * corner.  A phone that shows a 16:9 picture has
 * bars about that wide, so the column lies beside the picture; the D-pad
 * and the stick reach into it. */
typedef struct Place
{
	int right, bottom; // measured from the right edge (else the left), from the bottom (else the top)
	int dx, dy;        // the centre's distance from those edges
	int d;             // the control's size
} Place_t;
static const Place_t kPlace[OVERLAY_CONTROLS] = {
	{1, 0, 55, 55, 70},    // the pad's button
	{0, 0, 55, 55, 70},    // the settings button
	{0, 1, 135, 135, 240}, // the D-pad
	{0, 1, 135, 375, 200}, // the stick
	{1, 1, 80, 85, 130},   // Confirm
	{1, 1, 80, 215, 100},  // Cancel
	{1, 1, 80, 325, 80},   // Skip
	{1, 1, 80, 515, 80},   // Backlog
	{1, 1, 80, 420, 80},   // Forward
};
#define PAD_HEIGHT    560 // how far up the pad reaches from the bottom, at full size,
#define SHOW_HEIGHT   110 // .. and its button down from the top, with the room between the two
#define SCALE_MIN     128 // the pad at its smallest: half its size

/* The colours, 0xAARRGGBB.  A control is seen on the black of a bar and
 * on a picture of any colour: a grey glass that darkens a light picture
 * and lightens the black, a light rim, a light sign. */
#define COL_BODY      0x48606060u // a control at rest,
#define COL_BODY_HELD 0x90ffffffu // .. and under a finger;
#define COL_EDGE      0x80ffffffu // its rim,
#define COL_SIGN      0xd0ffffffu // .. and the sign on it
#define COL_SIGN_HELD 0xe0202020u

static OverlayState_t* Ov(OpenBGI_t* o)
{
	return &o->overlay;
}

static int Dpi(const OverlayState_t* v)
{
	return v->surface.dpi > 0 ? v->surface.dpi : DPI_UNKNOWN;
}

// the surface's free part
static void Safe(const OverlayState_t* v, int* x, int* y, int* w, int* h)
{
	const OpenBGI_Surface_t* s = &v->surface;
	*x = *y = 0;
	*w = s->width;
	*h = s->height;
	if(s->safe.w > 0 && s->safe.h > 0)
	{
		int x1 = s->safe.x + s->safe.w, y1 = s->safe.y + s->safe.h;
		*x = s->safe.x > 0 ? s->safe.x : 0;
		*y = s->safe.y > 0 ? s->safe.y : 0;
		*w = (x1 < s->width ? x1 : s->width) - *x;
		*h = (y1 < s->height ? y1 : s->height) - *y;
	}
}

// ---- the layer's pixels -----------------------------------------------------------------------------

void Overlay_Free(OpenBGI_t* o)
{
	OverlayState_t* v = Ov(o);
	BGI_Free(v->pix);
	v->pix = NULL;
	v->w = v->h = 0;
	v->laid = 0;
	v->handed = 0;
}

// a part of the layer changed
static void Changed(OverlayState_t* v, int x, int y, int w, int h)
{
	if(v->x1 <= v->x0)
	{
		v->x0 = x;
		v->y0 = y;
		v->x1 = x + w;
		v->y1 = y + h;
		return;
	}
	if(x < v->x0)
		v->x0 = x;
	if(y < v->y0)
		v->y0 = y;
	if(x + w > v->x1)
		v->x1 = x + w;
	if(y + h > v->y1)
		v->y1 = y + h;
}

// ---- drawing ----------------------------------------------------------------------------------------

enum
{
	SHAPE_DISC,  // p: the centre, the radius
	SHAPE_LINE,  // p: from, to, the width
	SHAPE_BOX,   // p: the centre, half the width, half the height, the corners' radius
	SHAPE_CROSS, // p: the centre, half an arm's width, half its length, the corners' radius
	SHAPE_TRI    // p: three corners
};

static float Len(float x, float y)
{
	return (float)sqrt((double)(x * x + y * y));
}

// t kept within 0 .. 1
static float Unit(float t)
{
	if(t < 0)
		return 0;
	return t > 1 ? 1 : t;
}

// how far (x, y) is outside the shape; negative inside
static float Distance(int shape, const float* p, float x, float y)
{
	switch(shape)
	{
		case SHAPE_DISC: return Len(x - p[0], y - p[1]) - p[2];
		case SHAPE_LINE:
		{
			float ax = x - p[0], ay = y - p[1], bx = p[2] - p[0], by = p[3] - p[1], bb = bx * bx + by * by;
			float t = bb > 0 ? (ax * bx + ay * by) / bb : 0;
			t = Unit(t);
			return Len(ax - bx * t, ay - by * t) - p[4] * 0.5f;
		}
		case SHAPE_BOX:
		{
			float qx = (float)fabs((double)(x - p[0])) - p[2] + p[4], qy = (float)fabs((double)(y - p[1])) - p[3] + p[4];
			float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;
			return Len(ox, oy) + (in < 0 ? in : 0) - p[4];
		}
		case SHAPE_CROSS:
		{ // two boxes as one
			float a[5] = {p[0], p[1], p[2], p[3], p[4]}, b[5] = {p[0], p[1], p[3], p[2], p[4]};
			float da = Distance(SHAPE_BOX, a, x, y), db = Distance(SHAPE_BOX, b, x, y);
			return da < db ? da : db;
		}
		default:
		{ // the triangle: inside when on the same side of all three edges; its distance the nearest edge's
			float d = 1e9f, side = 0;
			int i, in = 1;
			for(i = 0; i < 3; i++)
			{
				float ax = p[i * 2], ay = p[i * 2 + 1], bx = p[(i + 1) % 3 * 2], by = p[(i + 1) % 3 * 2 + 1];
				float ex = bx - ax, ey = by - ay, px = x - ax, py = y - ay, ee = ex * ex + ey * ey;
				float t = ee > 0 ? (px * ex + py * ey) / ee : 0, c = ex * py - ey * px, e;
				t = Unit(t);
				e = Len(px - ex * t, py - ey * t);
				if(e < d)
					d = e;
				if(i == 0)
					side = c;
				else if((c < 0) != (side < 0))
					in = 0;
			}
			return in ? -d : d;
		}
	}
}

/* A shape in colour `argb` over what control k's square holds, clipped to
 * that square - all of it, or with `line` > 0 its outline that wide.
 * (bx0, by0) .. (bx1, by1) is what of the layer the shape can reach. */
static void Paint(OverlayState_t* v, int k, int shape, const float* p, float line, uint32_t argb, float bx0, float by0, float bx1, float by1)
{
	const OverlayControl_t* c = &v->c[k];
	int x0 = (int)floor((double)bx0) - 1, y0 = (int)floor((double)by0) - 1, x1 = (int)ceil((double)bx1) + 1, y1 = (int)ceil((double)by1) + 1, x, y;
	unsigned sa = argb >> 24, sr = (argb >> 16) & 0xff, sg = (argb >> 8) & 0xff, sb = argb & 0xff;
	if(x0 < c->x)
		x0 = c->x;
	if(y0 < c->y)
		y0 = c->y;
	if(x1 > c->x + c->side)
		x1 = c->x + c->side;
	if(y1 > c->y + c->side)
		y1 = c->y + c->side;
	if(x0 < 0)
		x0 = 0;
	if(y0 < 0)
		y0 = 0;
	if(x1 > v->w)
		x1 = v->w;
	if(y1 > v->h)
		y1 = v->h;
	for(y = y0; y < y1; y++)
	{
		uint8_t* d = v->pix + ((size_t)y * (size_t)v->w + (size_t)x0) * 4;
		for(x = x0; x < x1; x++, d += 4)
		{
			float dist = Distance(shape, p, (float)x + 0.5f, (float)y + 0.5f);
			float cover = 0.5f - (line > 0 ? (float)fabs((double)dist) - line * 0.5f : dist);
			unsigned a, da, keep, out;
			if(cover <= 0)
				continue;
			a = (unsigned)((cover > 1 ? 1 : cover) * (float)sa + 0.5f);
			if(!a)
				continue;
			// over, with the alpha that is there: the colours are kept apart from it
			da = d[3];
			keep = da * (255 - a) / 255;
			out = a + keep;
			d[0] = (uint8_t)((sb * a + d[0] * keep) / out);
			d[1] = (uint8_t)((sg * a + d[1] * keep) / out);
			d[2] = (uint8_t)((sr * a + d[2] * keep) / out);
			d[3] = (uint8_t)out;
		}
	}
}

static void Disc(OverlayState_t* v, int k, float cx, float cy, float r, uint32_t argb)
{
	float p[3] = {cx, cy, r};
	Paint(v, k, SHAPE_DISC, p, 0, argb, cx - r, cy - r, cx + r, cy + r);
}

static void Ring(OverlayState_t* v, int k, float cx, float cy, float r, float width, uint32_t argb)
{
	float p[3] = {cx, cy, r}, e = r + width;
	Paint(v, k, SHAPE_DISC, p, width, argb, cx - e, cy - e, cx + e, cy + e);
}

static void Line(OverlayState_t* v, int k, float x0, float y0, float x1, float y1, float width, uint32_t argb)
{
	float p[5] = {x0, y0, x1, y1, width};
	Paint(v, k, SHAPE_LINE, p, 0, argb, (x0 < x1 ? x0 : x1) - width, (y0 < y1 ? y0 : y1) - width, (x0 > x1 ? x0 : x1) + width, (y0 > y1 ? y0 : y1) + width);
}

static void Box(OverlayState_t* v, int k, float cx, float cy, float hw, float hh, float corner, uint32_t argb)
{
	float p[5] = {cx, cy, hw, hh, corner};
	Paint(v, k, SHAPE_BOX, p, 0, argb, cx - hw, cy - hh, cx + hw, cy + hh);
}

static void BoxLine(OverlayState_t* v, int k, float cx, float cy, float hw, float hh, float corner, float line, uint32_t argb)
{
	float p[5] = {cx, cy, hw, hh, corner};
	Paint(v, k, SHAPE_BOX, p, line, argb, cx - hw - line, cy - hh - line, cx + hw + line, cy + hh + line);
}

// a cross: its arms `arm` wide each way from the middle and `reach` long; `line` > 0: its outline
static void Cross(OverlayState_t* v, int k, float cx, float cy, float arm, float reach, float corner, float line, uint32_t argb)
{
	float p[5] = {cx, cy, arm, reach, corner}, e = reach + line;
	Paint(v, k, SHAPE_CROSS, p, line, argb, cx - e, cy - e, cx + e, cy + e);
}

static void Tri(OverlayState_t* v, int k, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t argb)
{
	float p[6] = {x0, y0, x1, y1, x2, y2};
	float l = x0 < x1 ? x0 : x1, t = y0 < y1 ? y0 : y1, r = x0 > x1 ? x0 : x1, b = y0 > y1 ? y0 : y1;
	Paint(v, k, SHAPE_TRI, p, 0, argb, l < x2 ? l : x2, t < y2 ? t : y2, r > x2 ? r : x2, b > y2 ? b : y2);
}

// a triangle that points along (dx, dy), its middle at (cx, cy), `s` its size
static void Arrow(OverlayState_t* v, int k, float cx, float cy, float dx, float dy, float s, uint32_t argb)
{
	float tx = cx + dx * s * 0.6f, ty = cy + dy * s * 0.6f, bx = cx - dx * s * 0.4f, by = cy - dy * s * 0.4f;
	Tri(v, k, tx, ty, bx - dy * s * 0.6f, by + dx * s * 0.6f, bx + dy * s * 0.6f, by - dx * s * 0.6f, argb);
}

#define PERF_ALPHA 0xd8 // how much the performance display's panel covers of the game under it

/* A part of the layer as it is without the controls: the performance
 * display's panel where it reaches into the part, and over it the
 * settings' picture, or nothing (the layer's colours are not multiplied
 * by their alpha: a pixel of the settings that is half clear over the
 * panel, which is itself a little clear). */
static void Base(OverlayState_t* v, int x, int y, int w, int h)
{
	int row, x0, y0, x1, y1, col;
	for(row = y; row < y + h; row++)
	{
		uint8_t* d = v->pix + ((size_t)row * (size_t)v->w + (size_t)x) * 4;
		if(v->under)
			memcpy(d, v->under + (size_t)row * (size_t)v->underPitch + (size_t)x * 4, (size_t)w * 4);
		else
			memset(d, 0, (size_t)w * 4);
	}
	if(!v->perf)
		return;
	x0 = x > v->perfX ? x : v->perfX;
	y0 = y > v->perfY ? y : v->perfY;
	x1 = x + w < v->perfX + v->perfW ? x + w : v->perfX + v->perfW;
	y1 = y + h < v->perfY + v->perfH ? y + h : v->perfY + v->perfH;
	for(row = y0; row < y1; row++)
	{
		uint8_t* d = v->pix + ((size_t)row * (size_t)v->w + (size_t)x0) * 4;
		const uint32_t* s = v->perf + (size_t)(row - v->perfY) * (size_t)v->perfW + (size_t)(x0 - v->perfX);
		for(col = x0; col < x1; col++, d += 4, s++)
		{
			uint32_t over = d[3], rest = PERF_ALPHA * (255 - over) / 255, a = over + rest;
			if(over == 255)
				continue; // (the settings cover it)
			d[0] = (uint8_t)((d[0] * over + (*s & 0xff) * rest) / a);
			d[1] = (uint8_t)((d[1] * over + (*s >> 8 & 0xff) * rest) / a);
			d[2] = (uint8_t)((d[2] * over + (*s >> 16 & 0xff) * rest) / a);
			d[3] = (uint8_t)a;
		}
	}
}

// control k's square made anew - what lies under the controls, then the control as it is now
static void Draw(OverlayState_t* v, int k, int settingsUp)
{
	const OverlayControl_t* c = &v->c[k];
	int held = (v->held >> k) & 1, i;
	float cx = (float)c->cx + 0.5f, cy = (float)c->cy + 0.5f, r = (float)c->r, edge = r / 14 > 1.5f ? r / 14 : 1.5f;
	uint32_t body = held ? COL_BODY_HELD : COL_BODY, sign = held ? COL_SIGN_HELD : COL_SIGN;
	if(c->side <= 0 || c->x < 0 || c->y < 0 || c->x + c->side > v->w || c->y + c->side > v->h)
		return; // (no room for it on this surface)
	Base(v, c->x, c->y, c->side, c->side);
	Changed(v, c->x, c->y, c->side, c->side);
	if(k > OVERLAY_MENU && !v->padOn)
		return;
	switch(k)
	{
		case OVERLAY_MENU:
		{ // a cog wheel: eight teeth round a ring; light while the settings are up
			float in = r * 0.3f, out = r * 0.56f, line = r * 0.15f;
			if(settingsUp && !held)
			{
				body = COL_BODY_HELD;
				sign = COL_SIGN_HELD;
			}
			Disc(v, k, cx, cy, r - edge, body);
			Ring(v, k, cx, cy, r - edge, edge, COL_EDGE);
			for(i = 0; i < 8; i++)
			{
				float a = (float)i * 0.78539816f, dx = (float)cos((double)a), dy = (float)sin((double)a);
				Line(v, k, cx + dx * in, cy + dy * in, cx + dx * out, cy + dy * out, line, sign);
			}
			Ring(v, k, cx, cy, in, line, sign);
			break;
		}
		case OVERLAY_SHOW:
		{ // a small pad: its body, a cross on the left, two buttons on the right; filled while the pad is shown
			float bw = r * 0.6f, bh = r * 0.36f, line = r * 0.1f;
			Disc(v, k, cx, cy, r - edge, body);
			Ring(v, k, cx, cy, r - edge, edge, COL_EDGE);
			if(v->padOn && !held)
			{
				Box(v, k, cx, cy, bw, bh, bh * 0.7f, sign);
				sign = COL_SIGN_HELD;
			}
			else
				BoxLine(v, k, cx, cy, bw, bh, bh * 0.7f, line, sign);
			Line(v, k, cx - bw * 0.72f, cy, cx - bw * 0.22f, cy, line, sign);
			Line(v, k, cx - bw * 0.47f, cy - bh * 0.42f, cx - bw * 0.47f, cy + bh * 0.42f, line, sign);
			Disc(v, k, cx + bw * 0.32f, cy + bh * 0.2f, line * 0.75f, sign);
			Disc(v, k, cx + bw * 0.64f, cy - bh * 0.2f, line * 0.75f, sign);
			break;
		}
		case OVERLAY_DPAD:
		{ // a cross, an arrow on each arm; the arm under the finger is the light one
			static const int kDir[4] = {OPENBGI_DPAD_UP, OPENBGI_DPAD_DOWN, OPENBGI_DPAD_LEFT, OPENBGI_DPAD_RIGHT};
			static const float kTo[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
			float arm = r / 3, reach = r - edge;
			int i;
			Cross(v, k, cx, cy, arm, reach, arm * 0.35f, 0, COL_BODY);
			for(i = 0; i < 4; i++)
			{
				int on = (v->dirs & kDir[i]) != 0;
				float ax = cx + kTo[i][0] * r * 0.66f, ay = cy + kTo[i][1] * r * 0.66f;
				if(on)
					Box(v, k, ax, ay, kTo[i][0] != 0 ? reach - r * 0.66f : arm, kTo[i][1] != 0 ? reach - r * 0.66f : arm, arm * 0.3f, COL_BODY_HELD);
				Arrow(v, k, ax, ay, kTo[i][0], kTo[i][1], r * 0.2f, on ? COL_SIGN_HELD : COL_SIGN);
			}
			Cross(v, k, cx, cy, arm, reach, arm * 0.35f, edge, COL_EDGE);
			break;
		}
		case OVERLAY_STICK:
		{ // a dish with the knob where the finger has it
			float travel = r * 0.45f, kx = cx + (float)v->stickX / 32767.0f * travel, ky = cy + (float)v->stickY / 32767.0f * travel;
			Disc(v, k, cx, cy, r - edge, COL_BODY);
			Ring(v, k, cx, cy, r - edge, edge, COL_EDGE);
			Disc(v, k, kx, ky, r * 0.45f, held ? COL_BODY_HELD : COL_EDGE);
			break;
		}
		default:
			Disc(v, k, cx, cy, r - edge, body);
			Ring(v, k, cx, cy, r - edge, edge, COL_EDGE);
			if(k == OVERLAY_CONFIRM)
				Ring(v, k, cx, cy, r * 0.36f, r * 0.13f, sign);
			else if(k == OVERLAY_CANCEL)
			{
				Line(v, k, cx - r * 0.3f, cy - r * 0.3f, cx + r * 0.3f, cy + r * 0.3f, r * 0.15f, sign);
				Line(v, k, cx - r * 0.3f, cy + r * 0.3f, cx + r * 0.3f, cy - r * 0.3f, r * 0.15f, sign);
			}
			else if(k == OVERLAY_SKIP)
			{
				Arrow(v, k, cx - r * 0.2f, cy, 1, 0, r * 0.36f, sign);
				Arrow(v, k, cx + r * 0.26f, cy, 1, 0, r * 0.36f, sign);
			}
			else
				Arrow(v, k, cx, cy, 0, k == OVERLAY_BACKLOG ? -1.0f : 1.0f, r * 0.42f, sign);
			break;
	}
}

// ---- where the controls lie -------------------------------------------------------------------------

static void Layout(OverlayState_t* v)
{
	int sx, sy, sw, sh, dpi = Dpi(v), k, heightTenths, room;
	Safe(v, &sx, &sy, &sw, &sh);
	// the pad at its full size where the surface is high enough for it under its button, else as much smaller
	heightTenths = (int)((long long)sh * 254 / dpi);
	room = heightTenths - SHOW_HEIGHT;
	v->scale = room >= PAD_HEIGHT ? 256 : room * 256 / PAD_HEIGHT;
	if(v->scale < SCALE_MIN)
		v->scale = SCALE_MIN;
	for(k = 0; k < OVERLAY_CONTROLS; k++)
	{
		const Place_t* p = &kPlace[k];
		OverlayControl_t* c = &v->c[k];
		int scale = k <= OVERLAY_MENU ? 256 : v->scale;
		// tenths of a millimetre into pixels, at the control's scale
		int dx = (int)((long long)p->dx * dpi * scale / (254 * 256)), dy = (int)((long long)p->dy * dpi * scale / (254 * 256));
		int half;
		c->r = (int)((long long)p->d * dpi * scale / (254 * 256 * 2));
		if(c->r < 6)
			c->r = 6;
		c->cx = p->right ? sx + sw - 1 - dx : sx + dx;
		c->cy = p->bottom ? sy + sh - 1 - dy : sy + dy;
		// its square: a little more than the control (the stick's knob stays within its dish)
		half = c->r + 2;
		c->x = c->cx - half;
		c->y = c->cy - half;
		c->side = half * 2 + 1;
	}
	v->laid = 1;
}

// the control at a point of the surface: the nearest of those the point is on, -1 for none
static int Hit(const OverlayState_t* v, int x, int y)
{
	int k, best = -1;
	long long bestD = 0;
	for(k = 0; k < OVERLAY_CONTROLS && !v->bare; k++)
	{
		const OverlayControl_t* c = &v->c[k];
		long long dx = x - c->cx, dy = y - c->cy, d = dx * dx + dy * dy, reach = c->r + c->r / 5 + 2;
		if(k > OVERLAY_MENU && !v->padOn)
			continue;
		if(k == OVERLAY_DPAD ? llabs(dx) > c->r || llabs(dy) > c->r : d > reach * reach)
			continue;
		if(best < 0 || d < bestD)
		{
			best = k;
			bestD = d;
		}
	}
	return best;
}

// ---- the pad ----------------------------------------------------------------------------------------

// the controls as a pad's state, to the mapper's own port
static void PadSend(OpenBGI_t* o)
{
	OverlayState_t* v = Ov(o);
	OpenBGI_PadState_t s;
	memset(&s, 0, sizeof s);
	s.size = sizeof s;
	if(v->held & 1u << OVERLAY_CONFIRM)
		s.buttons |= OPENBGI_PAD_SOUTH;
	if(v->held & 1u << OVERLAY_CANCEL)
		s.buttons |= OPENBGI_PAD_EAST;
	if(v->held & 1u << OVERLAY_SKIP)
		s.buttons |= OPENBGI_PAD_R2;
	if(v->held & 1u << OVERLAY_BACKLOG)
		s.buttons |= OPENBGI_PAD_L1;
	if(v->held & 1u << OVERLAY_FORWARD)
		s.buttons |= OPENBGI_PAD_R1;
	s.dpad = (uint32_t)v->dirs;
	s.leftX = (int16_t)v->stickX;
	s.leftY = (int16_t)v->stickY;
	if(!Settings_Pad(o, &s))
		Pad_Port(o, PAD_PORT_OWN, &s);
}

// nothing is held on the pad any more, whoever had it; the fingers that held something do nothing more
static void Release(OpenBGI_t* o)
{
	OverlayState_t* v = Ov(o);
	OpenBGI_PadState_t none;
	int k;
	for(k = 0; k < OVERLAY_TOUCHES; k++)
		if(v->t[k].used && v->t[k].control > OVERLAY_MENU)
			v->t[k].control = -2;
	v->held &= 1u << OVERLAY_SHOW | 1u << OVERLAY_MENU;
	v->dirs = v->stickX = v->stickY = 0;
	v->stale = (1u << OVERLAY_CONTROLS) - 1;
	memset(&none, 0, sizeof none);
	none.size = sizeof none;
	Pad_Port(o, PAD_PORT_OWN, &none);
	Settings_Pad(o, &none);
}

void Overlay_PanelChanged(OpenBGI_t* o)
{
	OverlayState_t* v = Ov(o);
	int k;
	for(k = 0; k < OVERLAY_TOUCHES; k++)
		if(v->t[k].used && v->t[k].control == -1)
		{ // a finger on the game's picture when the settings came: taken away; one on the settings when they went has nobody to tell
			if(Settings_Up(o))
				OpenBGI_Touch(o, v->t[k].id, OPENBGI_TOUCH_CANCEL, 0, 0);
			v->t[k].control = -2;
		}
	Release(o);
	v->underAll = 1;
}

int Overlay_Controls(OpenBGI_t* o)
{
	return !MainWin_ViewOn(o);
}

int Overlay_PadOn(OpenBGI_t* o)
{
	return Ov(o)->padOn;
}

void Overlay_SetPad(OpenBGI_t* o, int on, int keep)
{
	OverlayState_t* v = Ov(o);
	on = on != 0;
	if(v->padOn != on)
	{
		v->padOn = on;
		Release(o); // nothing is held on a pad that comes or goes
	}
	if(keep)
	{
		Engine_CfgSet(o, "", "touchPad", 0, on ? "on" : NULL);
		if(o->launcher.running)
			Launcher_SettingsChanged(o); // (its frames write them)
		else
			Engine_SaveGames(o);
	}
}

int Overlay_Through(OpenBGI_t* o)
{
	return !Ov(o)->padOnly;
}

void Overlay_SetThrough(OpenBGI_t* o, int on)
{
	OverlayState_t* v = Ov(o);
	v->padOnly = !on;
	Engine_CfgSet(o, "", "touchThrough", 0, on ? NULL : "off");
	if(o->launcher.running)
		Launcher_SettingsChanged(o);
	else
		Engine_SaveGames(o);
}

// a finger on the D-pad or the stick at (x, y): what it means there
static void Aim(OpenBGI_t* o, int k, int x, int y)
{
	OverlayState_t* v = Ov(o);
	const OverlayControl_t* c = &v->c[k];
	int dx = x - c->cx, dy = y - c->cy;
	if(k == OVERLAY_DPAD)
	{ // the arm the finger is nearest to, once it is off the middle
		int dirs = 0;
		if(abs(dx) > c->r / 6 || abs(dy) > c->r / 6)
			dirs = abs(dx) > abs(dy) ? (dx < 0 ? OPENBGI_DPAD_LEFT : OPENBGI_DPAD_RIGHT) : (dy < 0 ? OPENBGI_DPAD_UP : OPENBGI_DPAD_DOWN);
		if(dirs == v->dirs)
			return;
		v->dirs = dirs;
	}
	else
	{ // the stick at its end when the finger is three quarters of the way out, wherever it goes from there
		int reach = c->r * 3 / 4 > 0 ? c->r * 3 / 4 : 1;
		long long sx = (long long)dx * 32767 / reach, sy = (long long)dy * 32767 / reach, len2 = sx * sx + sy * sy;
		if(len2 > 32767ll * 32767)
		{
			double len = sqrt((double)len2);
			sx = (long long)((double)sx * 32767 / len);
			sy = (long long)((double)sy * 32767 / len);
		}
		if((int)sx == v->stickX && (int)sy == v->stickY)
			return;
		v->stickX = (int)sx;
		v->stickY = (int)sy;
	}
	v->stale |= 1u << k;
	PadSend(o);
}

// ---- the host's calls -------------------------------------------------------------------------------

void OpenBGI_SetSurface(OpenBGI_t* o, const OpenBGI_Surface_t* surface)
{
	OverlayState_t* v = Ov(o);
	OpenBGI_Surface_t s;
	int k;
	if(!surface || !Host_Take(&s, sizeof s, surface) || s.width <= 0 || s.height <= 0)
		memset(&s, 0, sizeof s);
	if(memcmp(&s, &v->surface, sizeof s) == 0)
		return;
	if(s.width != v->surface.width || s.height != v->surface.height || s.dpi != v->surface.dpi || memcmp(&s.safe, &v->surface.safe, sizeof s.safe) != 0)
		v->laid = 0; // (the picture's place alone moves no control)
	k = (s.width > 0) != (v->surface.width > 0);
	v->surface = s;
	v->ppiSet = -1;
	if(k)
		MainWin_ViewRescale(o); // (the settings' filter holds where the picture is laid on a surface)
}

void Overlay_ToPicture(OpenBGI_t* o, int sx, int sy, int* px, int* py)
{
	const OpenBGI_Rect_t* p = &Ov(o)->surface.picture;
	int w = o->screen.pic.w, h = o->screen.pic.h;
	long long x, y;
	*px = sx;
	*py = sy;
	if(p->w <= 0 || p->h <= 0 || w <= 0 || h <= 0)
		return;
	// (rounded down, also left of the picture and over it)
	x = (long long)(sx - p->x) * w;
	y = (long long)(sy - p->y) * h;
	*px = (int)(x >= 0 ? x / p->w : -((-x + p->w - 1) / p->w));
	*py = (int)(y >= 0 ? y / p->h : -((-y + p->h - 1) / p->h));
}

static OverlayTouch_t* TouchOf(OverlayState_t* v, int id, int make)
{
	int k, free = -1;
	for(k = 0; k < OVERLAY_TOUCHES; k++)
	{
		if(v->t[k].used && v->t[k].id == id)
			return &v->t[k];
		if(!v->t[k].used && free < 0)
			free = k;
	}
	if(!make || free < 0)
		return NULL;
	v->t[free].used = 1;
	v->t[free].id = id;
	v->t[free].control = -2;
	return &v->t[free];
}

void OpenBGI_SurfaceTouch(OpenBGI_t* o, int id, int phase, int x, int y)
{
	OverlayState_t* v = Ov(o);
	OverlayTouch_t* t;
	int px, py, k;
	if(v->surface.width <= 0)
	{ // no surface was said: the picture's own pixels
		OpenBGI_Touch(o, id, phase, x, y);
		return;
	}
	if(!v->laid)
		Layout(v);
	Overlay_ToPicture(o, x, y, &px, &py);
	if(phase == OPENBGI_TOUCH_DOWN)
	{
		t = TouchOf(v, id, 1);
		if(!t)
			return;
		k = Hit(v, x, y);
		if(k >= 0 && !(v->held & 1u << k))
		{ // the control's, until it lifts
			t->control = k;
			v->held |= 1u << k;
			v->stale |= 1u << k;
			if(k == OVERLAY_DPAD || k == OVERLAY_STICK)
				Aim(o, k, x, y);
			else if(k > OVERLAY_MENU)
				PadSend(o);
		}
		else if(k < 0 && (Settings_Up(o) || !(v->padOnly && v->padOn)))
		{ // the picture's - or the settings', whose picture is the surface's size
			t->control = -1;
			if(!Settings_Touch(o, id, phase, x, y))
				OpenBGI_Touch(o, id, phase, px, py);
		}
		return;
	}
	t = TouchOf(v, id, 0);
	if(!t)
		return;
	k = t->control;
	if(k == -1)
	{
		if(!Settings_Touch(o, id, phase, x, y))
			OpenBGI_Touch(o, id, phase, px, py);
	}
	else if(k >= 0 && phase == OPENBGI_TOUCH_MOVE)
	{
		if(k == OVERLAY_DPAD || k == OVERLAY_STICK)
			Aim(o, k, x, y);
	}
	else if(k >= 0)
	{ // lifted, or taken away
		v->held &= ~(1u << k);
		v->stale |= 1u << k;
		if(k == OVERLAY_DPAD)
			v->dirs = 0;
		if(k == OVERLAY_STICK)
			v->stickX = v->stickY = 0;
		if(k == OVERLAY_SHOW)
		{ // the button does its work when the finger lifts on it
			if(phase == OPENBGI_TOUCH_UP && Hit(v, x, y) == OVERLAY_SHOW)
				Overlay_SetPad(o, !v->padOn, 1);
		}
		else if(k == OVERLAY_MENU)
		{
			if(phase == OPENBGI_TOUCH_UP && Hit(v, x, y) == OVERLAY_MENU)
			{
				if(Settings_Up(o))
					Settings_Close(o);
				else
					Settings_Open(o);
			}
		}
		else
			PadSend(o);
	}
	if(phase != OPENBGI_TOUCH_MOVE)
		t->used = 0;
}

// ---- the frame --------------------------------------------------------------------------------------

// what the settings say of the pad, once they are read (the game runs)
static void Load(OpenBGI_t* o)
{
	OverlayState_t* v = Ov(o);
	char value[16];
	v->loaded = 1;
	v->padOnly = Engine_CfgGet(o, "", "touchThrough", 0, value, sizeof value) > 0 && strcmp(value, "off") == 0;
	Overlay_SetPad(o, Engine_CfgGet(o, "", "touchPad", 0, value, sizeof value) > 0 && strcmp(value, "on") == 0, 0);
}

/* What lies under the controls, into the layer: the settings' picture
 * where it changed - all of it when the settings came or the layer is
 * new, nothing but clear pixels when they went - and every control whose
 * square that reaches is drawn anew. */
static void Under(OpenBGI_t* o)
{
	OverlayState_t* v = Ov(o);
	const OpenBGI_Rect_t* change = NULL;
	int w, h, pitch, changes, i, k;
	v->under = Settings_Picture(o, &w, &h, &pitch, &changes, &change);
	v->underPitch = pitch;
	if(v->under && (w != v->w || h != v->h))
		v->under = NULL; // (a picture of another size: of a surface that was, until its program follows)
	if(v->underAll || (v->under != NULL) != v->underWas)
	{
		Base(v, 0, 0, v->w, v->h);
		v->stale = (1u << OVERLAY_CONTROLS) - 1;
		v->all = 1;
	}
	else if(v->under && changes)
	{
		OpenBGI_Rect_t whole = {0, 0, v->w, v->h};
		if(changes < 0)
		{
			change = &whole;
			changes = 1;
		}
		for(i = 0; i < changes; i++)
		{
			int x0 = change[i].x, y0 = change[i].y, x1 = x0 + change[i].w, y1 = y0 + change[i].h;
			if(x0 < 0)
				x0 = 0;
			if(y0 < 0)
				y0 = 0;
			if(x1 > v->w)
				x1 = v->w;
			if(y1 > v->h)
				y1 = v->h;
			if(x1 <= x0 || y1 <= y0)
				continue;
			Base(v, x0, y0, x1 - x0, y1 - y0);
			Changed(v, x0, y0, x1 - x0, y1 - y0);
			for(k = 0; k < OVERLAY_CONTROLS; k++)
			{
				const OverlayControl_t* c = &v->c[k];
				if(x0 < c->x + c->side && c->x < x1 && y0 < c->y + c->side && c->y < y1)
					v->stale |= 1u << k;
			}
		}
	}
	v->underWas = v->under != NULL;
	v->underAll = 0;
}

// a part of the layer made anew under the controls (clipped to the layer): the controls it reaches are drawn anew after it
static void Renew(OverlayState_t* v, int x, int y, int w, int h)
{
	int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > v->w ? v->w : x + w, y1 = y + h > v->h ? v->h : y + h, k;
	if(x1 <= x0 || y1 <= y0)
		return;
	Base(v, x0, y0, x1 - x0, y1 - y0);
	Changed(v, x0, y0, x1 - x0, y1 - y0);
	for(k = 0; k < OVERLAY_CONTROLS; k++)
	{
		const OverlayControl_t* c = &v->c[k];
		if(x0 < c->x + c->side && c->x < x1 && y0 < c->y + c->side && c->y < y1)
			v->stale |= 1u << k;
	}
}

/* Where the performance display's panel of w x h lies: at the top of the
 * surface's free part, to the right of the settings button and of the
 * pad's left hand (the D-pad and the stick, shown or not: it does not move
 * when the pad comes); on a surface too narrow for that (a phone held
 * upright), under the two buttons. */
static void PerfPlace(const OverlayState_t* v, int w, int h, int* x, int* y)
{
	static const int kLeft[] = {OVERLAY_MENU, OVERLAY_DPAD, OVERLAY_STICK};
	const OverlayControl_t* m = &v->c[OVERLAY_MENU];
	int sx, sy, sw, sh, gap = Dpi(v) * 20 / 254, i;
	Safe(v, &sx, &sy, &sw, &sh);
	BGI_UNUSED(h);
	*x = sx;
	for(i = 0; i < 3 && !v->bare; i++)
		if(v->c[kLeft[i]].side > 0 && v->c[kLeft[i]].x + v->c[kLeft[i]].side > *x)
			*x = v->c[kLeft[i]].x + v->c[kLeft[i]].side;
	*x += gap;
	*y = sy + gap;
	if(*x + w > sx + sw)
	{
		*x = sx + (sw - w) / 2;
		*y = v->bare ? sy + gap : m->y + m->side + gap;
	}
	if(*x < 0)
		*x = 0;
	if(*y < 0)
		*y = 0;
}

void Overlay_Frame(OpenBGI_t* o, OpenBGI_Frame_t* f)
{
	OverlayState_t* v = Ov(o);
	const OpenBGI_Surface_t* s = &v->surface;
	int k, pw = 0, ph = 0, px = 0, py = 0, perfChanged = 0, oldX, oldY, oldW, oldH;
	if(s->width <= 0 || s->height <= 0)
	{
		Settings_Close(o); // (no layer to show them in)
		if(v->pix)
			Overlay_Free(o);
		return;
	}
	if(!v->loaded && o->engine.stage == ENGINE_STAGE_VM)
		Load(o);
	v->bare = !Overlay_Controls(o);
	if(v->bare && !Settings_Up(o) && !Perf_On(o))
	{ // nothing to show: no layer
		if(v->pix)
			Overlay_Free(o);
		return;
	}
	// the gestures' distances are physical: the picture's pixels an inch, as it lies on the surface
	if(s->picture.w > 0 && o->screen.pic.w > 0)
	{
		int ppi = (int)(((long long)Dpi(v) * o->screen.pic.w + s->picture.w / 2) / s->picture.w);
		if(ppi != v->ppiSet)
		{
			v->ppiSet = ppi;
			OpenBGI_TouchScale(o, ppi);
		}
	}
	if(!v->pix || v->w != s->width || v->h != s->height)
	{
		BGI_Free(v->pix);
		v->pix = (uint8_t*)BGI_Calloc((size_t)s->width * (size_t)s->height * 4);
		v->w = s->width;
		v->h = s->height;
		v->laid = 0;
		v->handed = 0;
	}
	if(!v->laid)
	{ // the controls somewhere else: all of the layer anew
		Layout(v);
		v->underAll = 1;
	}
	// the performance display's panel as it is now (its pixels are good for this frame), and where it was
	oldX = v->perfX;
	oldY = v->perfY;
	oldW = v->perfShown ? v->perfW : 0;
	oldH = v->perfShown ? v->perfH : 0;
	v->perf = Perf_Picture(o, Dpi(v), &pw, &ph, &perfChanged);
	if(v->perf)
		PerfPlace(v, pw, ph, &px, &py);
	if((v->perf != NULL) != v->perfShown || px != oldX || py != oldY || pw != oldW || ph != oldH)
		perfChanged = 1;
	v->perfX = px;
	v->perfY = py;
	v->perfW = pw;
	v->perfH = ph;
	v->perfShown = v->perf != NULL;
	Under(o);
	if(perfChanged)
	{ // where it was, and where it is
		Renew(v, oldX, oldY, oldW, oldH);
		Renew(v, px, py, pw, ph);
	}
	for(k = 0; k < OVERLAY_CONTROLS && !v->bare; k++)
		if(v->stale & 1u << k)
			Draw(v, k, Settings_Up(o));
	v->stale = 0;
	f->layer = v->pix;
	f->layerWidth = v->w;
	f->layerHeight = v->h;
	f->layerPitch = v->w * 4;
	if(v->all || !v->handed)
		f->layerChanges = -1;
	else if(v->x1 > v->x0)
	{
		f->layerChanges = 1;
		f->layerChange.x = v->x0;
		f->layerChange.y = v->y0;
		f->layerChange.w = v->x1 - v->x0;
		f->layerChange.h = v->y1 - v->y0;
	}
	v->handed = 1;
	v->all = 0;
	v->x0 = v->x1 = v->y0 = v->y1 = 0;
	v->under = NULL; // (the settings' next frame makes its picture anew)
	v->perf = NULL;  // (and the display's pixels are its own again)
}
