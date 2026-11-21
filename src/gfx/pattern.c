/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pattern.c - gray patterns made by rule (src/include/bgi/gfx/pattern.h)
 *
 * The ramps are a place t between 0 and UNIT that Ramp turns into a gray
 * level; the tiles read the bitmap as it was; the polygon is sampled on
 * SUB lines per pixel row and exactly along each of them, the line is a
 * distance, and a triangle is the three weights of its corners.  Chance is
 * a hash of the place and the seed, so that a pattern comes out the same
 * every time.
 */
#include "bgi/gfx/pattern.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define UNIT      65536 // a whole ramp
#define SUB       16    // lines sampled per pixel row of a polygon

#define ROW(b, y) ((b)->pixels + (size_t)(y) * (size_t)(b)->pitch)

static const double kPi = 3.14159265358979323846;

static int Byte(int v)
{
	return v < 0 ? 0 : v > 255 ? 255
							   : v;
}

// what p makes of d in `mode`, as far as `cover` of 255
static inline uint8_t Mix(int d, int p, int cover, int mode)
{
	int r;
	switch(mode)
	{
		case PAT_LIGHTEN: r = d > p ? d : p; break;
		case PAT_DARKEN: r = d < p ? d : p; break;
		case PAT_ADD: r = d + p > 255 ? 255 : d + p; break;
		case PAT_SUBTRACT: r = d - p < 0 ? 0 : d - p; break;
		case PAT_MULTIPLY: r = d * p / 255; break;
		default: r = p; break;
	}
	return (uint8_t)(d + (r - d) * cover / 255);
}

// ---- ramps -------------------------------------------------------------------------------------

typedef struct Ramp
{
	int v0, v1, repeat, flags;
} Ramp_t;

// a ramp from the four values v0, v1, repeat, flags
static void Ramp_Take(Ramp_t* r, const int32_t* p)
{
	r->v0 = Byte(p[0]);
	r->v1 = Byte(p[1]);
	r->repeat = p[2] < 1 ? 1 : p[2] > 4096 ? 4096
										   : p[2];
	r->flags = p[3];
}

// the gray level of the ramp at t (0 .. UNIT) for the pixel (x, y)
static inline int Ramp_At(const Ramp_t* r, int64_t t, int x, int y)
{
	// the thresholds of an ordered dither, 0 .. 15
	static const uint8_t kDither[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
	int64_t f;
	int v;
	if(t < 0)
		t = 0;
	if(t > UNIT)
		t = UNIT;
	f = t * r->repeat;
	f = f >= (int64_t)UNIT * r->repeat ? UNIT : f % UNIT;
	if(r->flags & PAT_MIRROR)
		f = UNIT - llabs(2 * f - UNIT);
	if(r->flags & PAT_SMOOTH)
		f = ((f * f >> 16) * (3 * UNIT - 2 * f)) >> 16;
	v = r->v0 * 256 + (int)(((int64_t)(r->v1 - r->v0) * f) >> 8); // with 8 more bits
	v += r->flags & PAT_DITHER ? kDither[y & 3][x & 3] * 16 + 8 : 128;
	return Byte(v >> 8);
}

static int Linear(Bmp_t* b, int mode, const int32_t* p)
{
	Ramp_t r;
	double a = (double)p[0] / 65536.0 * kPi / 180.0;
	int64_t c = llround(cos(a) * UNIT), s = llround(sin(a) * UNIT);
	int64_t reach = (int64_t)b->w * llabs(c) + (int64_t)b->h * llabs(s); // twice how far the corners lie from the centre along the direction
	int x, y;
	Ramp_Take(&r, p + 1);
	if(reach <= 0)
		return 0;
	for(y = 0; y < b->h; y++)
	{
		uint8_t* row = ROW(b, y);
		for(x = 0; x < b->w; x++)
		{
			int64_t along = (int64_t)(2 * x + 1 - b->w) * c + (int64_t)(2 * y + 1 - b->h) * s;
			row[x] = Mix(row[x], Ramp_At(&r, (along + reach) * UNIT / (2 * reach), x, y), 255, mode);
		}
	}
	return 1;
}

static int Radial(Bmp_t* b, int mode, const int32_t* p)
{
	Ramp_t r;
	int x, y;
	Ramp_Take(&r, p + 4);
	if(p[2] <= 0 || p[3] <= 0)
		return 0;
	for(y = 0; y < b->h; y++)
	{
		uint8_t* row = ROW(b, y);
		double fy = ((double)(y - p[1]) + 0.5) / (double)p[3];
		for(x = 0; x < b->w; x++)
		{
			double fx = ((double)(x - p[0]) + 0.5) / (double)p[2], d = sqrt(fx * fx + fy * fy);
			row[x] = Mix(row[x], Ramp_At(&r, d >= 1 ? UNIT : (int64_t)(d * UNIT), x, y), 255, mode);
		}
	}
	return 1;
}

static int Sweep(Bmp_t* b, int mode, const int32_t* p)
{
	Ramp_t r;
	double start = (double)p[2] / 65536.0;
	int x, y;
	Ramp_Take(&r, p + 3);
	for(y = 0; y < b->h; y++)
	{
		uint8_t* row = ROW(b, y);
		for(x = 0; x < b->w; x++)
		{
			double a = fmod(atan2((double)(y - p[1]) + 0.5, (double)(x - p[0]) + 0.5) * 180.0 / kPi - start, 360.0);
			if(a < 0)
				a += 360.0;
			row[x] = Mix(row[x], Ramp_At(&r, (int64_t)(a / 360.0 * UNIT), x, y), 255, mode);
		}
	}
	return 1;
}

// ---- noise -------------------------------------------------------------------------------------

static uint32_t Hash(uint32_t a, uint32_t b, uint32_t seed)
{
	uint32_t h = (seed + 0x9E3779B9u) * 0x85EBCA6Bu ^ a * 0xC2B2AE35u ^ b * 0x27D4EB2Fu;
	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	h ^= h >> 12;
	h *= 0x297A2D39u;
	h ^= h >> 15;
	return h;
}

/* noise at (u, v), UNIT to a cell: 0 .. 65535.  A `wrap` above 0 is the
 * number of cells after which the noise comes round again along u. */
static int Noise_At(int64_t u, int64_t v, uint32_t seed, int wrap, int smooth)
{
	int64_t iu = u >> 16, iv = v >> 16, fu = u & 0xffff, fv = v & 0xffff, top, bottom;
	uint32_t u0 = (uint32_t)(wrap > 0 ? iu % wrap : iu), u1 = (uint32_t)(wrap > 0 ? (iu + 1) % wrap : iu + 1);
	int a = (int)(Hash(u0, (uint32_t)iv, seed) & 0xffff), bb = (int)(Hash(u1, (uint32_t)iv, seed) & 0xffff);
	int c = (int)(Hash(u0, (uint32_t)iv + 1, seed) & 0xffff), d = (int)(Hash(u1, (uint32_t)iv + 1, seed) & 0xffff);
	if(smooth)
	{
		fu = ((fu * fu >> 16) * (3 * UNIT - 2 * fu)) >> 16;
		fv = ((fv * fv >> 16) * (3 * UNIT - 2 * fv)) >> 16;
	}
	top = a + (((int64_t)(bb - a) * fu) >> 16);
	bottom = c + (((int64_t)(d - c) * fu) >> 16);
	return (int)(top + (((bottom - top) * fv) >> 16));
}

static int Noise(Bmp_t* b, int mode, const int32_t* p)
{
	Ramp_t r;
	uint32_t seed = (uint32_t)p[0];
	int cellW = p[1], cellH = p[2], octaves = p[5] < 1 ? 1 : p[5] > 6 ? 6
																	  : p[5],
		flags = p[6];
	int wrap = 0, x, y, k;
	if(cellW <= 0 || cellH <= 0)
		return 0;
	r.v0 = Byte(p[3]);
	r.v1 = Byte(p[4]);
	r.repeat = 1;
	r.flags = flags & PAT_DITHER;
	if(flags & PAT_POLAR)
	{ // a whole number of cells to the turn
		wrap = (360 + cellW / 2) / cellW;
		if(wrap < 1)
			wrap = 1;
	}
	for(y = 0; y < b->h; y++)
	{
		uint8_t* row = ROW(b, y);
		for(x = 0; x < b->w; x++)
		{
			int64_t u, v, sum = 0, weights = 0;
			if(flags & PAT_POLAR)
			{
				double dx = (double)x + 0.5 - (double)b->w / 2, dy = (double)y + 0.5 - (double)b->h / 2;
				double a = atan2(dy, dx) * 180.0 / kPi;
				if(a < 0)
					a += 360.0;
				u = (int64_t)(a / 360.0 * (double)wrap * UNIT);
				v = (int64_t)(sqrt(dx * dx + dy * dy) * UNIT / (double)cellH);
			}
			else
			{
				u = ((int64_t)x * UNIT + UNIT / 2) / cellW;
				v = ((int64_t)y * UNIT + UNIT / 2) / cellH;
			}
			for(k = 0; k < octaves; k++)
			{
				int weight = 1 << (octaves - 1 - k);
				sum += (int64_t)Noise_At(u << k, v << k, seed + (uint32_t)k * 0x632BE5ABu, wrap << k, flags & PAT_SMOOTH) * weight;
				weights += weight;
			}
			row[x] = Mix(row[x], Ramp_At(&r, sum / weights, x, y), 255, mode);
		}
	}
	return 1;
}

// ---- tiles -------------------------------------------------------------------------------------

typedef struct Tile
{
	double cx, cy; // its centre
	uint32_t id;   // its own chance number
} Tile_t;

// the tile of `shape` that (x, y) lies in
static void Tile_At(int shape, double x, double y, double w, double h, uint32_t seed, Tile_t* t)
{
	double i = floor(x / w), j = floor(y / h);
	switch(shape)
	{
		case PAT_TILE_COLUMNS:
			t->cx = (i + 0.5) * w;
			t->cy = y;
			t->id = Hash((uint32_t)(int32_t)i, 0x10001u, seed);
			break;
		case PAT_TILE_ROWS:
			t->cx = x;
			t->cy = (j + 0.5) * h;
			t->id = Hash(0x10001u, (uint32_t)(int32_t)j, seed);
			break;
		case PAT_TILE_BRICKS:
		{
			double shift = ((int32_t)j & 1) ? w / 2 : 0;
			i = floor((x + shift) / w);
			t->cx = (i + 0.5) * w - shift;
			t->cy = (j + 0.5) * h;
			t->id = Hash((uint32_t)(int32_t)i, (uint32_t)(int32_t)j, seed);
			break;
		}
		case PAT_TILE_TRIANGLES:
		{
			double fx = x / w - i, fy = y / h - j, third;
			int turned = ((int32_t)i + (int32_t)j) & 1, lower;
			if(turned)
				fx = 1 - fx;
			lower = fx + fy >= 1;
			third = lower ? 2.0 / 3.0 : 1.0 / 3.0;
			t->cx = (i + (turned ? 1 - third : third)) * w;
			t->cy = (j + third) * h;
			t->id = Hash((uint32_t)(int32_t)i * 2u + (uint32_t)lower, (uint32_t)(int32_t)j, seed);
			break;
		}
		case PAT_TILE_DIAMONDS:
		{
			double a = floor(x / w + y / h) + 0.5, c = floor(x / w - y / h) + 0.5;
			t->cx = w * (a + c) / 2;
			t->cy = h * (a - c) / 2;
			t->id = Hash((uint32_t)(int32_t)floor(a), (uint32_t)(int32_t)floor(c), seed);
			break;
		}
		case PAT_TILE_HEXAGONS:
		{
			double size = w / sqrt(3.0); // from the centre to a corner; the corners point up and down
			double q = (sqrt(3.0) / 3.0 * x - y / 3.0) / size, r = 2.0 / 3.0 * y / size, s = -q - r;
			double rq = floor(q + 0.5), rr = floor(r + 0.5), rs = floor(s + 0.5);
			double dq = fabs(rq - q), dr = fabs(rr - r), ds = fabs(rs - s);
			if(dq > dr && dq > ds)
				rq = -rr - rs;
			else if(dr > ds)
				rr = -rq - rs;
			t->cx = size * sqrt(3.0) * (rq + rr / 2);
			t->cy = size * 1.5 * rr;
			t->id = Hash((uint32_t)(int32_t)rq, (uint32_t)(int32_t)rr, seed);
			break;
		}
		case PAT_TILE_CELLS:
		{
			double best = 0;
			int di, dj, first = 1;
			for(dj = -1; dj <= 1; dj++)
				for(di = -1; di <= 1; di++)
				{ // the cell belongs to the nearest of the points, one by chance to every w x h pixels
					uint32_t id = Hash((uint32_t)((int32_t)i + di), (uint32_t)((int32_t)j + dj), seed);
					double sx = (i + di + (double)(id & 0xfff) / 4096.0) * w, sy = (j + dj + (double)(id >> 12 & 0xfff) / 4096.0) * h;
					double d = (sx - x) * (sx - x) + (sy - y) * (sy - y);
					if(first || d < best)
					{
						best = d;
						first = 0;
						t->cx = sx;
						t->cy = sy;
						t->id = Hash(id, 0x5bd1e995u, seed);
					}
				}
			break;
		}
		default: // PAT_TILE_RECT
			t->cx = (i + 0.5) * w;
			t->cy = (j + 0.5) * h;
			t->id = Hash((uint32_t)(int32_t)i, (uint32_t)(int32_t)j, seed);
			break;
	}
}

static int Tiles(Bmp_t* b, const int32_t* p)
{
	int shape = p[0], jitter = p[3], inner = p[4], flags = p[7], x, y;
	double w = (double)p[1], h = (double)p[2], size, fixed = (double)p[5] / 65536.0 * kPi / 180.0;
	uint32_t seed = (uint32_t)p[6];
	uint8_t* was;
	if(shape < PAT_TILE_RECT || shape > PAT_TILE_CELLS || p[1] <= 0 || (p[2] <= 0 && shape != PAT_TILE_HEXAGONS && shape != PAT_TILE_COLUMNS))
		return 0;
	if(p[2] <= 0)
		h = w;
	size = shape == PAT_TILE_COLUMNS ? w : shape == PAT_TILE_ROWS ? h
		: w > h                                                   ? w
																  : h;
	was = (uint8_t*)malloc((size_t)b->w * (size_t)b->h);
	if(!was)
		return 0;
	for(y = 0; y < b->h; y++)
		memcpy(was + (size_t)y * (size_t)b->w, ROW(b, y), (size_t)b->w);
	for(y = 0; y < b->h; y++)
	{
		uint8_t* row = ROW(b, y);
		for(x = 0; x < b->w; x++)
		{
			Tile_t t;
			int sx, sy;
			double v;
			Tile_At(shape, (double)x + 0.5, (double)y + 0.5, w, h, seed, &t);
			sx = (int)floor(t.cx);
			sy = (int)floor(t.cy);
			sx = sx < 0 ? 0 : sx >= b->w ? b->w - 1
										 : sx;
			sy = sy < 0 ? 0 : sy >= b->h ? b->h - 1
										 : sy;
			v = (double)was[(size_t)sy * (size_t)b->w + (size_t)sx];
			if(jitter)
				v += ((double)(t.id & 0xffff) / 32767.5 - 1.0) * (double)jitter;
			if(inner)
			{
				double a = flags & PAT_TURNED ? (double)(t.id >> 16) / 65536.0 * 2.0 * kPi : fixed;
				double along = (((double)x + 0.5 - t.cx) * cos(a) + ((double)y + 0.5 - t.cy) * sin(a)) / size;
				v += (double)inner * (along < -0.5 ? -0.5 : along > 0.5 ? 0.5
																		: along);
			}
			row[x] = (uint8_t)Byte((int)floor(v + 0.5));
		}
	}
	free(was);
	return 1;
}

// ---- shapes ------------------------------------------------------------------------------------

// an edge of a polygon, top to bottom: where it starts and ends (1/256 pixel), and which way it ran
typedef struct Edge
{
	int32_t x0, y0, x1, y1;
	int dir;
} Edge_t;

static int Polygon(Bmp_t* b, int mode, int value, const int32_t* points, int n)
{
	Edge_t* edges;
	int32_t* acc; // per pixel of a row: the change of the coverage there, and the part of a pixel a span's ends cover
	int32_t* part;
	int count = 0, i, y, s;
	int32_t top = INT32_MAX, bottom = INT32_MIN;
	if(n < 3 || n > PAT_POINTS_MAX)
		return 0;
	edges = (Edge_t*)malloc(sizeof *edges * (size_t)n);
	acc = (int32_t*)calloc((size_t)b->w + 2, sizeof *acc * 2);
	if(!edges || !acc)
	{
		free(edges);
		free(acc);
		return 0;
	}
	part = acc + b->w + 2;
	for(i = 0; i < n; i++)
	{
		const int32_t* p = points + i * 2;
		const int32_t* q = points + ((i + 1) % n) * 2;
		if(p[1] == q[1])
			continue; // a level edge crosses no line
		edges[count].dir = p[1] < q[1] ? 1 : -1;
		edges[count].x0 = p[1] < q[1] ? p[0] : q[0];
		edges[count].y0 = p[1] < q[1] ? p[1] : q[1];
		edges[count].x1 = p[1] < q[1] ? q[0] : p[0];
		edges[count].y1 = p[1] < q[1] ? q[1] : p[1];
		if(edges[count].y0 < top)
			top = edges[count].y0;
		if(edges[count].y1 > bottom)
			bottom = edges[count].y1;
		count++;
	}
	for(y = top < 0 ? 0 : top / PAT_ONE; count && y < b->h && (int64_t)y * PAT_ONE < bottom; y++)
	{
		uint8_t* row = ROW(b, y);
		int touched = 0, x;
		int64_t run = 0;
		for(s = 0; s < SUB; s++)
		{
			int32_t sy = y * PAT_ONE + (s * 2 + 1) * PAT_ONE / (SUB * 2);
			int32_t xs[64];
			int8_t dirs[64];
			int k = 0, j, wind = 0;
			int32_t from = 0;
			for(i = 0; i < count && k < 64; i++)
			{
				const Edge_t* e = &edges[i];
				int32_t cx;
				if(sy < e->y0 || sy >= e->y1)
					continue;
				cx = e->x0 + (int32_t)((int64_t)(e->x1 - e->x0) * (sy - e->y0) / (e->y1 - e->y0));
				for(j = k; j > 0 && xs[j - 1] > cx; j--)
				{
					xs[j] = xs[j - 1];
					dirs[j] = dirs[j - 1];
				}
				xs[j] = cx;
				dirs[j] = (int8_t)e->dir;
				k++;
			}
			for(j = 0; j < k; j++)
			{
				int was = wind;
				wind += dirs[j];
				if(!was && wind)
					from = xs[j];
				else if(was && !wind)
				{ // inside from `from` to here
					int32_t xa = from < 0 ? 0 : from, xb = xs[j] > b->w * PAT_ONE ? b->w * PAT_ONE : xs[j];
					int ia, ib;
					if(xb <= xa)
						continue;
					ia = xa / PAT_ONE;
					ib = xb / PAT_ONE;
					touched = 1;
					if(ia == ib)
						part[ia] += xb - xa;
					else
					{
						part[ia] += PAT_ONE - (xa & (PAT_ONE - 1));
						part[ib] += xb & (PAT_ONE - 1);
						acc[ia + 1] += PAT_ONE;
						acc[ib] -= PAT_ONE;
					}
				}
			}
		}
		if(!touched)
			continue;
		for(x = 0; x < b->w; x++)
		{
			int cover;
			run += acc[x];
			cover = (int)((run + part[x]) * 255 / (PAT_ONE * SUB));
			acc[x] = part[x] = 0;
			if(cover > 0)
				row[x] = Mix(row[x], value, cover > 255 ? 255 : cover, mode);
		}
		acc[b->w] = part[b->w] = 0;
	}
	free(edges);
	free(acc);
	return 1;
}

static int Line(Bmp_t* b, int mode, int value, int width, int closed, const int32_t* points, int n)
{
	int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN, i, w, h, px, py;
	int segments = closed && n > 2 ? n : n - 1;
	double half = (double)width / (2.0 * PAT_ONE);
	uint8_t* cover;
	if(n < 1 || n > PAT_POINTS_MAX || width <= 0)
		return 0;
	for(i = 0; i < n; i++)
	{ // the pixels the line can reach
		int reach = (width + PAT_ONE) / (2 * PAT_ONE) + 2;
		int cx = points[i * 2] / PAT_ONE, cy = points[i * 2 + 1] / PAT_ONE;
		if(cx - reach < x0)
			x0 = cx - reach;
		if(cy - reach < y0)
			y0 = cy - reach;
		if(cx + reach > x1)
			x1 = cx + reach;
		if(cy + reach > y1)
			y1 = cy + reach;
	}
	if(x0 < 0)
		x0 = 0;
	if(y0 < 0)
		y0 = 0;
	if(x1 >= b->w)
		x1 = b->w - 1;
	if(y1 >= b->h)
		y1 = b->h - 1;
	if(x1 < x0 || y1 < y0)
		return 1;
	w = x1 - x0 + 1;
	h = y1 - y0 + 1;
	cover = (uint8_t*)calloc((size_t)w, (size_t)h); // the most any part of the line covers of a pixel: where two parts meet nothing doubles
	if(!cover)
		return 0;
	for(i = 0; i < (segments > 0 ? segments : 1); i++)
	{
		const int32_t* p = points + i * 2;
		const int32_t* q = points + (segments > 0 ? (i + 1) % n : i) * 2;
		double ax = (double)p[0] / PAT_ONE, ay = (double)p[1] / PAT_ONE, bx = (double)q[0] / PAT_ONE, by = (double)q[1] / PAT_ONE;
		double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
		int sx0 = (int)floor((ax < bx ? ax : bx) - half - 1), sx1 = (int)ceil((ax > bx ? ax : bx) + half + 1);
		int sy0 = (int)floor((ay < by ? ay : by) - half - 1), sy1 = (int)ceil((ay > by ? ay : by) + half + 1);
		if(sx0 < x0)
			sx0 = x0;
		if(sy0 < y0)
			sy0 = y0;
		if(sx1 > x1)
			sx1 = x1;
		if(sy1 > y1)
			sy1 = y1;
		for(py = sy0; py <= sy1; py++)
			for(px = sx0; px <= sx1; px++)
			{
				double cx = (double)px + 0.5 - ax, cy = (double)py + 0.5 - ay, t = len2 > 0 ? (cx * dx + cy * dy) / len2 : 0, d;
				int c;
				uint8_t* at = cover + (size_t)(py - y0) * (size_t)w + (size_t)(px - x0);
				if(t < 0)
					t = 0;
				if(t > 1)
					t = 1;
				cx -= dx * t;
				cy -= dy * t;
				d = half + 0.5 - sqrt(cx * cx + cy * cy);
				c = d <= 0 ? 0 : d >= 1 ? 255
										: (int)(d * 255.0);
				if(c > *at)
					*at = (uint8_t)c;
			}
	}
	for(py = 0; py < h; py++)
	{
		uint8_t* row = ROW(b, py + y0) + x0;
		const uint8_t* c = cover + (size_t)py * (size_t)w;
		for(px = 0; px < w; px++)
			if(c[px])
				row[px] = Mix(row[px], value, c[px], mode);
	}
	free(cover);
	return 1;
}

// twice the area of (a, b, p), above 0 when they turn clockwise on the screen
static int64_t Side(const int32_t* a, const int32_t* b, int64_t px, int64_t py)
{
	return (int64_t)(b[0] - a[0]) * (py - a[1]) - (int64_t)(b[1] - a[1]) * (px - a[0]);
}

// an edge whose pixels belong to its triangle: a level one along the top, or one on the left
static int Owned(const int32_t* a, const int32_t* b)
{
	return (a[1] == b[1] && b[0] > a[0]) || b[1] < a[1];
}

// a triangle from 9 values: x, y, v of its three corners
static void Triangle(Bmp_t* b, int mode, const int32_t* p)
{
	const int32_t* a = p;
	const int32_t* c1 = p + 3;
	const int32_t* c2 = p + 6;
	int64_t area = Side(a, c1, c2[0], c2[1]);
	int x, y, x0, y0, x1, y1, bias0, bias1, bias2;
	if(area == 0)
		return;
	if(area < 0)
	{ // the other way round
		const int32_t* swap = c1;
		c1 = c2;
		c2 = swap;
		area = -area;
	}
	x0 = a[0] < c1[0] ? a[0] : c1[0];
	x0 = (x0 < c2[0] ? x0 : c2[0]) / PAT_ONE - 1;
	y0 = a[1] < c1[1] ? a[1] : c1[1];
	y0 = (y0 < c2[1] ? y0 : c2[1]) / PAT_ONE - 1;
	x1 = a[0] > c1[0] ? a[0] : c1[0];
	x1 = (x1 > c2[0] ? x1 : c2[0]) / PAT_ONE + 1;
	y1 = a[1] > c1[1] ? a[1] : c1[1];
	y1 = (y1 > c2[1] ? y1 : c2[1]) / PAT_ONE + 1;
	x0 = x0 < 0 ? 0 : x0;
	y0 = y0 < 0 ? 0 : y0;
	x1 = x1 >= b->w ? b->w - 1 : x1;
	y1 = y1 >= b->h ? b->h - 1 : y1;
	bias0 = Owned(c1, c2) ? 0 : -1;
	bias1 = Owned(c2, a) ? 0 : -1;
	bias2 = Owned(a, c1) ? 0 : -1;
	for(y = y0; y <= y1; y++)
	{
		uint8_t* row = ROW(b, y);
		int64_t py = (int64_t)y * PAT_ONE + PAT_ONE / 2;
		for(x = x0; x <= x1; x++)
		{
			int64_t px = (int64_t)x * PAT_ONE + PAT_ONE / 2;
			int64_t w0 = Side(c1, c2, px, py), w1 = Side(c2, a, px, py), w2 = Side(a, c1, px, py);
			if(w0 + bias0 < 0 || w1 + bias1 < 0 || w2 + bias2 < 0)
				continue;
			row[x] = Mix(row[x], Byte((int)((w0 * a[2] + w1 * c1[2] + w2 * c2[2] + area / 2) / area)), 255, mode);
		}
	}
}

int Pattern_Generate(Bmp_t* b, int kind, int mode, const int32_t* p, int count)
{
	int i;
	if(!b || !b->pixels || b->w <= 0 || b->h <= 0 || b->mode != PM_GRAY8 || !p || mode < PAT_SET || mode > PAT_MULTIPLY)
		return 0;
	switch(kind)
	{
		case PAT_LINEAR: return count >= 5 && Linear(b, mode, p);
		case PAT_RADIAL: return count >= 8 && Radial(b, mode, p);
		case PAT_SWEEP: return count >= 7 && Sweep(b, mode, p);
		case PAT_NOISE: return count >= 7 && Noise(b, mode, p);
		case PAT_TILES: return count >= 8 && Tiles(b, p);
		case PAT_POLYGON: return count >= 7 && Polygon(b, mode, Byte(p[0]), p + 1, (count - 1) / 2);
		case PAT_LINE: return count >= 5 && Line(b, mode, Byte(p[0]), p[1], p[2], p + 3, (count - 3) / 2);
		case PAT_TRIANGLES:
			if(count < 9)
				return 0;
			for(i = 0; i + 9 <= count; i += 9)
				Triangle(b, mode, p + i);
			return 1;
		default: return 0;
	}
}
