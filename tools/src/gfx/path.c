/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * path.c - the motion path of "90 29": up to 100 control points in 16.16
 * coordinates, interpolated per axis by a natural cubic spline.  Interface
 * in inc/bgi/gfx/path.h.
 *
 * The path tween (wait/wait_tween.c) clears the path, adds the object's
 * current fixed position and the script's points, sets the parameter range
 * and then evaluates the path at the eased progress every frame.  The
 * splines are natural cubic splines over unit-spaced knots, built lazily
 * the first time a point is evaluated and rebuilt after every
 * Path_AddPoint.  All spline arithmetic is double, in the original's
 * operation order.
 */
#include <math.h>
#include "bgi/gfx/path.h"

// the coefficients of the natural cubic spline through v[0..count-1] (count >= 3)
static void Spline_Build(Spline_t* s, const int32_t* v, int count)
{
	double l[PATH_MAX_POINTS + 2]; // the forward-elimination multipliers
	int n = count - 1, i;

	s->n = n;
	for(i = 0; i <= n; i++)
		s->y[i] = (double)v[i];
	s->c[n] = 0.0;
	s->c[0] = 0.0;
	for(i = 1; i < n; i++)
		s->c[i] = (s->y[i - 1] - (s->y[i] + s->y[i]) + s->y[i + 1]) * 3.0;
	// tridiagonal system with 4 on the diagonal (Thomas algorithm)
	l[1] = 0.0;
	for(i = 1; i < n; i++)
	{
		double m = 1.0 / (4.0 - l[i]);
		s->c[i] = (s->c[i] - s->c[i - 1]) * m;
		l[i + 1] = m;
	}
	for(i = n - 1; i > 0; i--)
		s->c[i] = s->c[i] - l[i + 1] * s->c[i + 1];
	s->d[n] = 0.0;
	s->b[n] = 0.0;
	for(i = 0; i < n; i++)
	{
		s->d[i] = (s->c[i + 1] - s->c[i]) * 0.3333333333333333;
		s->b[i] = s->y[i + 1] - s->y[i] - s->c[i] - s->d[i];
	}
}

// the spline at parameter t in 0..1 (the knots sit at i / n)
static double Spline_Eval(const Spline_t* s, double t)
{
	double x = (double)s->n * t, u;
	int i = BGI_Ftol(floor(x));
	if(i < 0)
		i = 0;
	else if(i >= s->n)
		i = s->n - 1;
	u = x - (double)i;
	return ((s->d[i] * u + s->c[i]) * u + s->b[i]) * u + s->y[i];
}

Path_t* Path_New(void)
{
	Path_t* p = (Path_t*)BGI_Calloc(sizeof(Path_t));
	Path_Clear(p);
	return p;
}

void Path_Delete(Path_t* p)
{
	BGI_Free(p);
}

// no points, no range
void Path_Clear(Path_t* p)
{
	p->rangeLo = p->rangeHi = 0;
	p->built = 0;
	p->count = 0;
}

// append a point; 0 when 100 points are stored already
int Path_AddPoint(Path_t* p, int32_t x, int32_t y, int32_t z)
{
	if(p->count >= PATH_MAX_POINTS)
		return 0;
	p->built = 0;
	p->x[p->count] = x;
	p->y[p->count] = y;
	p->z[p->count] = z;
	p->count++;
	return 1;
}

// the parameter range of Path_Eval: `lo` is the start, `hi` the length (the tween uses 0 and 0x10000)
void Path_SetRange(Path_t* p, uint32_t lo, uint32_t hi)
{
	p->rangeLo = lo;
	p->rangeHi = hi;
}

// three or more points: the splines, built on first use
static int Path_EvalSpline(Path_t* p, double u, int32_t out[3])
{
	if(!p->built)
	{
		p->built = 1;
		Spline_Build(&p->sx, p->x, p->count);
		Spline_Build(&p->sy, p->y, p->count);
		Spline_Build(&p->sz, p->z, p->count);
	}
	out[0] = BGI_Ftol(Spline_Eval(&p->sx, u));
	out[1] = BGI_Ftol(Spline_Eval(&p->sy, u));
	out[2] = BGI_Ftol(Spline_Eval(&p->sz, u));
	return 1;
}

/* The point at parameter t: the first point at t = lo, the last at
 * t = lo + hi (and beyond).  One point is constant, two interpolate
 * linearly, more go through the splines.  1 ok, 0 (out zeroed) without
 * points, range or output. */
int Path_Eval(Path_t* p, uint32_t t, int32_t out[3])
{
	double u;
	int n = p->count;
	if(!out)
		return 0;
	out[0] = out[1] = out[2] = 0;
	if(p->rangeHi == 0 || n == 0)
		return 0;
	if(p->rangeLo + p->rangeHi <= t)
	{ // at or past the end: the last point
		out[0] = p->x[n - 1];
		out[1] = p->y[n - 1];
		out[2] = p->z[n - 1];
		return 1;
	}
	u = (double)(t - p->rangeLo) / (double)p->rangeHi;
	if(n == 1)
	{
		out[0] = p->x[0];
		out[1] = p->y[0];
		out[2] = p->z[0];
		return 1;
	}
	if(n == 2)
	{
		out[0] = p->x[0] + BGI_Ftol((double)(p->x[1] - p->x[0]) * u);
		out[1] = p->y[0] + BGI_Ftol((double)(p->y[1] - p->y[0]) * u);
		out[2] = p->z[0] + BGI_Ftol((double)(p->z[1] - p->z[0]) * u);
		return 1;
	}
	return Path_EvalSpline(p, u, out);
}
