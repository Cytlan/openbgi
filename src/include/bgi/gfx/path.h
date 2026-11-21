/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * path.h - the motion path of "90 29": up to 100 control points in 16.16
 * coordinates, interpolated per axis by a natural cubic spline
 * (src/gfx/path.c).
 */
#ifndef BGI_GFX_PATH_H_
#define BGI_GFX_PATH_H_

#include "bgi/common.h"

#define PATH_MAX_POINTS 100

typedef struct Spline // one axis
{
	int n;                         // intervals (points - 1)
	double y[PATH_MAX_POINTS + 1]; // knot values
	double b[PATH_MAX_POINTS + 1]; // cubic coefficients per unit interval
	double c[PATH_MAX_POINTS + 1];
	double d[PATH_MAX_POINTS + 1];
} Spline_t;

typedef struct Path
{
	int count; // the three coordinate vectors share one length
	int32_t x[PATH_MAX_POINTS];
	int32_t y[PATH_MAX_POINTS];
	int32_t z[PATH_MAX_POINTS];
	Spline_t sx, sy, sz;
	uint32_t rangeLo, rangeHi; // parameter range (Path_SetRange): start and length
	int built;                 // the splines match the points
} Path_t;

Path_t* Path_New(void); // allocated and cleared
void Path_Delete(Path_t* p);
void Path_Clear(Path_t* p);
int Path_AddPoint(Path_t* p, int32_t x, int32_t y, int32_t z); // 0 when 100 points are stored
void Path_SetRange(Path_t* p, uint32_t lo, uint32_t hi);
/* the point at parameter t (lo .. lo + hi); 1 ok, 0 without
 * points, range or output.  1 or 2 points interpolate linearly. */
int Path_Eval(Path_t* p, uint32_t t, int32_t out[3]);

#endif // BGI_GFX_PATH_H_
