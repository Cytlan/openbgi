/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * blit_rows.c - the rows of the commonest blits, twice: in word lanes
 *               and four pixels at a time (blit_simd.h); interface in
 *               bgi/gfx/bitmap.h (BlitRows_t)
 *
 * A game's frame is mostly these: a sprite over the picture (the alpha
 * copy), the same at a level (the blend), a scene change (the blend
 * without alpha, the cross fade) and light (the add).  blit.c walks the
 * rows of a blit and calls one of these for each.
 *
 * kBlitRowsPlain holds the word-lane loops, which run anywhere.
 * kBlitRows holds what the blitters use: the SSE2 or NEON loops of this
 * build, or the plain ones again where it has neither.  Both give the
 * same bytes for every row: tests/blitrows.c compares them, and
 * tests/blitgold.c holds the blitters to the sums of the first,
 * lane-by-lane code.
 *
 * A fast loop takes the pixels in fours and leaves the last one to three
 * of a row to the plain loop.  Each says what it asks of its arguments;
 * blit.c calls the plain loop when an argument is outside that.
 */
#include "blit_internal.h"
#include "blit_simd.h"

// ---- word lanes --------------------------------------------------------------------------------

// RGB32 <- ARGB32: d += (s - d) * (a >> 1) >> 7 for pixels with alpha >= 2; the top byte keeps d's value
static void Plain_AlphaCopy(uint32_t* d, const uint32_t* s, int n)
{
	int x;
	for(x = 0; x < n; x++)
	{
		uint32_t sv = s[x], dv = d[x];
		if(!(sv & 0xfe000000u))
			continue;
		d[x] = (dv & 0xff000000u) | Lerp7(dv, sv, sv >> 25);
	}
}

// RGB32 <- ARGB32 at a level: d += (s - d) * w >> 7 with w = ((a >> 1) * inv) >> 8, alpha >= 2 only
static void Plain_BlendAlpha(uint32_t* d, const uint32_t* s, int n, int inv)
{
	int x;
	for(x = 0; x < n; x++)
	{
		uint32_t sv = s[x], dv = d[x];
		if(!(sv & 0xfe000000u))
			continue;
		d[x] = (dv & 0xff000000u) | Lerp7(dv, sv, ((sv >> 25) * (uint32_t)inv) >> 8);
	}
}

// 32-bit <- 32-bit without alpha: every colour byte becomes s + ((d - s) * l2 >> 7); the top byte is the source's
static void Plain_BlendPlain(uint32_t* d, const uint32_t* s, int n, int l2)
{
	int x;
	for(x = 0; x < n; x++)
	{
		uint32_t sv = s[x];
		d[x] = (sv & 0xff000000u) | Lerp7(sv, d[x], (uint32_t)l2);
	}
}

/* RGB32 <- the cross fade of two ARGB32 rows: with va, vb the alphas
 * scaled by inv and ra their mix by `ratio`, the colour is the mix of
 * a * va and b * vb plus d * (256 - ra) / 256; nothing where ra is 0 */
static void Plain_Crossfade(uint32_t* d, const uint32_t* pa, const uint32_t* pb, int n, int ratio, int inv)
{
	int x;
	for(x = 0; x < n; x++)
	{
		uint32_t av = pa[x], bv = pb[x], dv = d[x];
		int va = ((int)(av >> 24) * inv) >> 8; // table 1: (alpha * inv) >> 8
		int vb = ((int)(bv >> 24) * inv) >> 8;
		int ra = va + (((vb - va) * ratio) >> 8); // alpha lane of the lerp
		int i, out, wd;
		if(ra == 0)
			continue;
		wd = 0x100 - ra;
		out = 0;
		for(i = 0; i < 24; i += 8)
		{
			int ac = (int)((av >> i) & 0xff), bc = (int)((bv >> i) & 0xff);
			int dc = (int)((dv >> i) & 0xff);
			int a1 = (ac * va) >> 8, b1 = (bc * vb) >> 8;
			int c = a1 + (((b1 - a1) * ratio) >> 8);
			c += (dc * wd) >> 8;
			out |= Sat8(c) << i;
		}
		d[x] = (uint32_t)out;
	}
}

/* 32-bit <- ARGB32, added: d += Sat8((s * w) >> 7) with w = ((a >> 1) *
 * level) >> 8 (a >> 1 from level 0x100 on), alpha >= 2 only.  An ARGB32
 * destination (withAlpha) adds min(255, 2 * w) to its alpha. */
static void Plain_AddAlpha(uint32_t* d, const uint32_t* s, int n, int level, int withAlpha)
{
	int x;
	for(x = 0; x < n; x++)
	{
		uint32_t sv = s[x], dv = d[x], out;
		int a = (int)(sv >> 25), w;
		if(!(sv & 0xfe000000u))
			continue;
		w = level >= 0x100 ? a : (a * level) >> 8;
		out = AddSat4(dv & 0x00ffffffu, Scale3(sv, (uint32_t)w, 7));
		if(withAlpha)
			out |= (uint32_t)Sat8((int)(dv >> 24) + Sat8(w << 1)) << 24;
		else
			out |= (int)(dv & 0xff000000u);
		d[x] = (uint32_t)out;
	}
}

// 32-bit <- RGB32, added: d += ((s * level) >> 8) | top on the four bytes, saturated, where the source is not black; level <= 0x100
static void Plain_AddPlain(uint32_t* d, const uint32_t* s, int n, int level, uint32_t top)
{
	int x;
	for(x = 0; x < n; x++)
	{
		uint32_t sv = s[x];
		if(sv & 0x00ffffffu)
			d[x] = AddSat4(d[x], Scale3(sv, (uint32_t)level, 8) | top);
	}
}

const BlitRows_t kBlitRowsPlain = {"none", Plain_AlphaCopy, Plain_BlendAlpha, Plain_BlendPlain, Plain_Crossfade, Plain_AddAlpha, Plain_AddPlain};

#if BLIT_SIMD

// ---- four pixels at a time ---------------------------------------------------------------------

// the colour lanes of two widened pixels (blue, green, red set; alpha clear), and the alpha lanes
static const uint32_t kColourLanes[4] = {0xffffffffu, 0x0000ffffu, 0xffffffffu, 0x0000ffffu};
static const uint32_t kAlphaLanes[4] = {0, 0xffff0000u, 0, 0xffff0000u};

// the low byte of each pixel on all four of its bytes: a weight under every lane once widened
#define SPREAD(w) \
	do \
	{ \
		(w) = V_Or((w), V_Shl32((w), 8)); \
		(w) = V_Or((w), V_Shl32((w), 16)); \
	} while(0)

// (x * (2^n - w) + y * w) >> n on eight lanes: x + (((y - x) * w) >> n) for bytes and 0 <= w <= 2^n (blit_internal.h)
#define MIX(x, y, w, whole, n) V_Shr16(V_Add16(V_Mul16((x), V_Sub16((whole), (w))), V_Mul16((y), (w))), (n))

// (the alpha lane gets the weight 0, so it keeps d's byte; clear pixels come out as they were)
static void Fast_AlphaCopy(uint32_t* d, const uint32_t* s, int n)
{
	const Vec k128 = V_Rep16(128), colour = V_Load(kColourLanes), seen = V_Rep32(0xfe000000u);
	int x;
	for(x = 0; x + 4 <= n; x += 4)
	{
		Vec sv = V_Load(s + x), dv, w, lo, hi;
		if(V_IsZero(V_And(sv, seen)))
			continue; // four clear pixels
		dv = V_Load(d + x);
		w = V_Shr32(sv, 25);
		SPREAD(w);
		lo = MIX(V_Lo16(dv), V_Lo16(sv), V_And(V_Lo16(w), colour), k128, 7);
		hi = MIX(V_Hi16(dv), V_Hi16(sv), V_And(V_Hi16(w), colour), k128, 7);
		V_Store(d + x, V_Pack(lo, hi));
	}
	Plain_AlphaCopy(d + x, s + x, n - x);
}

// 0 <= inv <= 0x100
static void Fast_BlendAlpha(uint32_t* d, const uint32_t* s, int n, int inv)
{
	const Vec k128 = V_Rep16(128), colour = V_Load(kColourLanes), seen = V_Rep32(0xfe000000u), scale = V_Rep16(inv);
	int x;
	for(x = 0; x + 4 <= n; x += 4)
	{
		Vec sv = V_Load(s + x), dv, w, lo, hi;
		if(V_IsZero(V_And(sv, seen)))
			continue;
		dv = V_Load(d + x);
		w = V_Shr32(sv, 25);
		SPREAD(w);
		lo = MIX(V_Lo16(dv), V_Lo16(sv), V_And(V_Shr16(V_Mul16(V_Lo16(w), scale), 8), colour), k128, 7);
		hi = MIX(V_Hi16(dv), V_Hi16(sv), V_And(V_Shr16(V_Mul16(V_Hi16(w), scale), 8), colour), k128, 7);
		V_Store(d + x, V_Pack(lo, hi));
	}
	Plain_BlendAlpha(d + x, s + x, n - x, inv);
}

// 0 <= l2 <= 128 (the destination's weight is 0 on the alpha lane: the source's byte stays)
static void Fast_BlendPlain(uint32_t* d, const uint32_t* s, int n, int l2)
{
	const Vec k128 = V_Rep16(128), w = V_And(V_Rep16(l2), V_Load(kColourLanes));
	int x;
	for(x = 0; x + 4 <= n; x += 4)
	{
		Vec sv = V_Load(s + x), dv = V_Load(d + x);
		Vec lo = MIX(V_Lo16(sv), V_Lo16(dv), w, k128, 7);
		Vec hi = MIX(V_Hi16(sv), V_Hi16(dv), w, k128, 7);
		V_Store(d + x, V_Pack(lo, hi));
	}
	Plain_BlendPlain(d + x, s + x, n - x, l2);
}

// one half of four cross-faded pixels: the lanes of a, b and d, and the two alphas under every lane
#define CROSSFADE_HALF(out, a16, b16, d16, aa, ba) \
	do \
	{ \
		Vec va = V_Shr16(V_Mul16((aa), scale), 8), vb = V_Shr16(V_Mul16((ba), scale), 8); \
		Vec ra = MIX(va, vb, part, k256, 8); \
		Vec c = MIX(V_Shr16(V_Mul16((a16), va), 8), V_Shr16(V_Mul16((b16), vb), 8), part, k256, 8); \
		c = V_Add16(c, V_Shr16(V_Mul16((d16), V_Sub16(k256, ra)), 8)); \
		(out) = V_Sel(V_Eq16(ra, V_Zero()), (d16), V_And(c, colour)); \
	} while(0)

// 0 <= ratio <= 0x100, 0 <= inv <= 0x100
static void Fast_Crossfade(uint32_t* d, const uint32_t* pa, const uint32_t* pb, int n, int ratio, int inv)
{
	const Vec k256 = V_Rep16(256), colour = V_Load(kColourLanes), top = V_Rep32(0xff000000u), scale = V_Rep16(inv), part = V_Rep16(ratio);
	int x;
	for(x = 0; x + 4 <= n; x += 4)
	{
		Vec av = V_Load(pa + x), bv = V_Load(pb + x), dv, aa, ba, lo, hi;
		if(V_IsZero(V_And(V_Or(av, bv), top)))
			continue; // both clear in all four
		dv = V_Load(d + x);
		aa = V_Shr32(av, 24);
		ba = V_Shr32(bv, 24);
		SPREAD(aa);
		SPREAD(ba);
		CROSSFADE_HALF(lo, V_Lo16(av), V_Lo16(bv), V_Lo16(dv), V_Lo16(aa), V_Lo16(ba));
		CROSSFADE_HALF(hi, V_Hi16(av), V_Hi16(bv), V_Hi16(dv), V_Hi16(aa), V_Hi16(ba));
		V_Store(d + x, V_Pack(lo, hi));
	}
	Plain_Crossfade(d + x, pa + x, pb + x, n - x, ratio, inv);
}

// one half of four added pixels: the colour lanes scaled by the weight, and twice the weight (or nothing) for the alpha lane
#define ADD_HALF(out, s16, a16) \
	do \
	{ \
		Vec w = level >= 0x100 ? (a16) : V_Shr16(V_Mul16((a16), scale), 8); \
		(out) = V_Or(V_And(V_Shr16(V_Mul16((s16), w), 7), colour), V_And(V_Add16(w, w), alpha)); \
	} while(0)

// 0 <= level
static void Fast_AddAlpha(uint32_t* d, const uint32_t* s, int n, int level, int withAlpha)
{
	const Vec colour = V_Load(kColourLanes), alpha = withAlpha ? V_Load(kAlphaLanes) : V_Zero(), seen = V_Rep32(0xfe000000u);
	const Vec scale = V_Rep16(level >= 0x100 ? 0 : level);
	int x;
	for(x = 0; x + 4 <= n; x += 4)
	{
		Vec sv = V_Load(s + x), a, lo, hi;
		if(V_IsZero(V_And(sv, seen)))
			continue;
		a = V_Shr32(sv, 25);
		SPREAD(a);
		ADD_HALF(lo, V_Lo16(sv), V_Lo16(a));
		ADD_HALF(hi, V_Hi16(sv), V_Hi16(a));
		V_Store(d + x, V_AddSat8(V_Load(d + x), V_Pack(lo, hi)));
	}
	Plain_AddAlpha(d + x, s + x, n - x, level, withAlpha);
}

// 0 <= level <= 0x100
static void Fast_AddPlain(uint32_t* d, const uint32_t* s, int n, int level, uint32_t top)
{
	const Vec colour = V_Load(kColourLanes), rgb = V_Rep32(0x00ffffffu), scale = V_Rep16(level), over = V_Rep32(top);
	int x;
	for(x = 0; x + 4 <= n; x += 4)
	{
		Vec sv = V_Load(s + x), black = V_Eq32(V_And(sv, rgb), V_Zero()), lo, hi;
		if(V_IsZero(V_And(sv, rgb)))
			continue; // four black pixels
		lo = V_And(V_Shr16(V_Mul16(V_Lo16(sv), scale), 8), colour);
		hi = V_And(V_Shr16(V_Mul16(V_Hi16(sv), scale), 8), colour);
		V_Store(d + x, V_AddSat8(V_Load(d + x), V_AndNot(black, V_Or(V_Pack(lo, hi), over))));
	}
	Plain_AddPlain(d + x, s + x, n - x, level, top);
}

const BlitRows_t kBlitRows = {BLIT_SIMD_NAME, Fast_AlphaCopy, Fast_BlendAlpha, Fast_BlendPlain, Fast_Crossfade, Fast_AddAlpha, Fast_AddPlain};

#else

const BlitRows_t kBlitRows = {"none", Plain_AlphaCopy, Plain_BlendAlpha, Plain_BlendPlain, Plain_Crossfade, Plain_AddAlpha, Plain_AddPlain};

#endif
