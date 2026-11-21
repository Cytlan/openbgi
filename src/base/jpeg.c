/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * jpeg.c - JPEG files for "90 C4" / "90 C5" (picture.h); the original
 *          uses GDI+.
 *
 * The decoder reads Huffman-coded files, baseline, extended (8-bit) and
 * progressive, with one (grey) or three components (YCbCr, or RGB when
 * an Adobe marker or the component names say so) at any sampling
 * factors, and restart intervals.  Every block's coefficients are kept
 * until the last scan; then each is dequantised and run through a
 * separable float inverse DCT, and subsampled components are scaled up
 * bilinearly with their samples centred, as the IJG library's "fancy"
 * upsampling does.  A truncated file gives what was decoded so far.
 *
 * The encoder writes what GDI+'s does: a baseline JFIF file with the IJG
 * quantisation tables scaled to the quality, 2x2 chroma subsampling and
 * the standard Huffman tables of the specification (annex K).
 */
#include "bgi/picture.h"
#include "bgi/bytes.h"

#include <math.h>
#include <string.h>

// the natural (row * 8 + column) index of the k-th coefficient in zigzag order
static const uint8_t kZigzag[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48,
	41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52,
	45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// the DCT basis: kCos[x][u] = C(u) / 2 * cos((2x + 1) u pi / 16), C(0) = 1 / sqrt(2), else 1
static float kCos[8][8];
static int gCosReady;

static void InitCos(void)
{
	int x, u;
	if(gCosReady)
		return;
	for(x = 0; x < 8; x++)
		for(u = 0; u < 8; u++)
			kCos[x][u] = (float)((u ? 0.5 : 0.5 / sqrt(2.0)) * cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0));
	gCosReady = 1;
}

static int Clamp255(float v)
{
	int i = (int)floorf(v + 0.5f);
	return i < 0 ? 0 : i > 255 ? 255
							   : i;
}

// ---- decoder ---------------------------------------------------------------------

typedef struct Huff
{
	int present;
	uint8_t vals[256];
	int32_t maxcode[18]; // the largest code of each length, -1 when there is none
	int32_t mincode[17];
	int32_t valptr[17];
} Huff_t;

typedef struct Comp
{
	int id;
	int h, v;      // the sampling factors
	int tq;        // the quantisation table
	int td, ta;    // the Huffman tables of the current scan
	int w, ht;     // the samples it covers (the image size scaled by its factors)
	int bw, bh;    // its blocks, padded to whole MCUs
	int16_t* coef; // bw * bh blocks of 64 coefficients, natural order
	uint8_t* pix;  // bw * 8 by bh * 8 samples
	int dcPred;
} Comp_t;

typedef struct Bits
{
	const uint8_t* p;
	const uint8_t* end;
	uint32_t buf; // the bits not yet used, at the top
	int n;        // how many
	int marker;   // a marker was reached: zeros are fed from now on
} Bits_t;

typedef struct Jpeg
{
	uint16_t q[4][64]; // natural order
	Huff_t dc[4], ac[4];
	Comp_t comp[4];
	int ncomp;
	int width, height;
	int hmax, vmax, mcux, mcuy;
	int progressive;
	int restart;
	int adobe, adobeTransform;
	int frame; // the frame header was seen
	int scans; // the scans decoded
	int eobrun;
} Jpeg_t;

static void Fill(Bits_t* b)
{
	while(b->n <= 24)
	{
		uint32_t c = 0;
		if(!b->marker && b->p < b->end)
		{
			c = *b->p;
			if(c == 0xFF)
			{
				uint32_t c2 = b->p + 1 < b->end ? b->p[1] : 0xD9;
				if(c2 == 0)
					b->p += 2; // a stuffed 0xFF
				else
				{
					b->marker = 1; // stay on the marker
					c = 0;
				}
			}
			else
				b->p++;
		}
		b->buf |= c << (24 - b->n);
		b->n += 8;
	}
}

static uint32_t GetBits(Bits_t* b, int n)
{
	uint32_t v;
	if(n == 0)
		return 0;
	Fill(b);
	v = b->buf >> (32 - n);
	b->buf <<= n;
	b->n -= n;
	return v;
}

static int GetBit(Bits_t* b)
{
	return (int)GetBits(b, 1);
}

// a `s`-bit magnitude as a signed value (a clear top bit is negative)
static int Extend(uint32_t v, int s)
{
	return s && v < (1u << (s - 1)) ? (int)v - (1 << s) + 1 : (int)v;
}

// the next symbol of table h; -1 for a code it does not have
static int Decode(Bits_t* b, const Huff_t* h)
{
	int32_t code = GetBit(b);
	int l = 1;
	while(l <= 16 && (h->maxcode[l] < 0 || code > h->maxcode[l]))
	{
		code = (code << 1) | GetBit(b);
		l++;
	}
	if(l > 16)
		return -1;
	return h->vals[h->valptr[l] + code - h->mincode[l]];
}

// DHT: one or more tables; 0 when damaged
static int ReadDht(Jpeg_t* j, const uint8_t* s, uint32_t len)
{
	while(len >= 17)
	{
		int tc = s[0] >> 4, th = s[0] & 15, l, total = 0;
		int32_t code = 0, k = 0;
		Huff_t* h;
		if(tc > 1 || th > 3)
			return 0;
		h = tc ? &j->ac[th] : &j->dc[th];
		for(l = 0; l < 16; l++)
			total += s[1 + l];
		if(total > 256 || len < 17u + (uint32_t)total)
			return 0;
		memcpy(h->vals, s + 17, (size_t)total);
		for(l = 1; l <= 16; l++)
		{
			int count = s[l];
			h->valptr[l] = k;
			h->mincode[l] = code;
			code += count;
			k += count;
			if(code > (1 << l))
				return 0;
			h->maxcode[l] = count ? code - 1 : -1;
			code <<= 1;
		}
		h->maxcode[17] = 0x7fffffff;
		h->present = 1;
		s += 17 + total;
		len -= 17u + (uint32_t)total;
	}
	return 1;
}

// DQT: 8- or 16-bit tables, zigzag order in the file
static int ReadDqt(Jpeg_t* j, const uint8_t* s, uint32_t len)
{
	while(len >= 65)
	{
		int pq = s[0] >> 4, tq = s[0] & 15, k;
		if(tq > 3 || pq > 1 || len < (pq ? 129u : 65u))
			return 0;
		for(k = 0; k < 64; k++)
			j->q[tq][kZigzag[k]] = (uint16_t)(pq ? Be16(s + 1 + 2 * k) : s[1 + k]);
		s += pq ? 129 : 65;
		len -= pq ? 129u : 65u;
	}
	return 1;
}

static int ReadSof(Jpeg_t* j, const uint8_t* s, uint32_t len)
{
	int i;
	if(j->frame || len < 6 || s[0] != 8)
		return 0; // one frame, 8-bit samples
	j->height = (int)Be16(s + 1);
	j->width = (int)Be16(s + 3);
	j->ncomp = s[5];
	if(j->width == 0 || j->height == 0 || (j->ncomp != 1 && j->ncomp != 3) || len < 6u + 3u * (uint32_t)j->ncomp)
		return 0;
	if((uint64_t)j->width * (uint64_t)j->height > 0x2000000u)
		return 0; // 32 M pixels at most
	j->hmax = j->vmax = 1;
	for(i = 0; i < j->ncomp; i++)
	{
		Comp_t* c = &j->comp[i];
		c->id = s[6 + 3 * i];
		c->h = s[7 + 3 * i] >> 4;
		c->v = s[7 + 3 * i] & 15;
		c->tq = s[8 + 3 * i];
		if(c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3)
			return 0;
		if(j->ncomp == 1)
			c->h = c->v = 1; // a lone component's MCU is one block whatever its factors
		if(c->h > j->hmax)
			j->hmax = c->h;
		if(c->v > j->vmax)
			j->vmax = c->v;
	}
	j->mcux = (j->width + 8 * j->hmax - 1) / (8 * j->hmax);
	j->mcuy = (j->height + 8 * j->vmax - 1) / (8 * j->vmax);
	for(i = 0; i < j->ncomp; i++)
	{
		Comp_t* c = &j->comp[i];
		c->w = (j->width * c->h + j->hmax - 1) / j->hmax;
		c->ht = (j->height * c->v + j->vmax - 1) / j->vmax;
		c->bw = j->mcux * c->h;
		c->bh = j->mcuy * c->v;
		c->coef = (int16_t*)BGI_Calloc((size_t)c->bw * (size_t)c->bh * 64 * sizeof(int16_t));
	}
	j->frame = 1;
	return 1;
}

// one block of a sequential scan; 0 when damaged
static int BlockSeq(Jpeg_t* j, Bits_t* b, Comp_t* c, int16_t* blk)
{
	int t = Decode(b, &j->dc[c->td]), k;
	if(t < 0 || t > 11)
		return 0;
	c->dcPred += Extend(GetBits(b, t), t);
	blk[0] = (int16_t)c->dcPred;
	for(k = 1; k < 64;)
	{
		int rs = Decode(b, &j->ac[c->ta]), r, s;
		if(rs < 0)
			return 0;
		r = rs >> 4;
		s = rs & 15;
		if(s == 0)
		{
			if(r != 15)
				break;
			k += 16;
			continue;
		}
		k += r;
		if(k > 63)
			return 0;
		blk[kZigzag[k]] = (int16_t)Extend(GetBits(b, s), s);
		k++;
	}
	return 1;
}

// one block of a progressive scan with spectral range ss .. se and the approximation ah / al
static int BlockProg(Jpeg_t* j, Bits_t* b, Comp_t* c, int16_t* blk, int ss, int se, int ah, int al)
{
	int k;
	if(ss == 0)
	{ // DC
		if(ah == 0)
		{
			int t = Decode(b, &j->dc[c->td]);
			if(t < 0 || t > 11)
				return 0;
			c->dcPred += Extend(GetBits(b, t), t);
			blk[0] = (int16_t)(c->dcPred * (1 << al));
		}
		else if(GetBit(b))
			blk[0] = (int16_t)(blk[0] | (1 << al));
		return 1;
	}
	if(ah == 0)
	{ // the first AC pass
		if(j->eobrun > 0)
		{
			j->eobrun--;
			return 1;
		}
		for(k = ss; k <= se;)
		{
			int rs = Decode(b, &j->ac[c->ta]), r, s;
			if(rs < 0)
				return 0;
			r = rs >> 4;
			s = rs & 15;
			if(s == 0)
			{
				if(r < 15)
				{
					j->eobrun = (1 << r) - 1;
					if(r)
						j->eobrun += (int)GetBits(b, r);
					break;
				}
				k += 16;
				continue;
			}
			k += r;
			if(k > 63)
				return 0;
			blk[kZigzag[k]] = (int16_t)(Extend(GetBits(b, s), s) * (1 << al));
			k++;
		}
		return 1;
	}
	{ // AC refinement: a bit for every coefficient already non-zero, new ones of +-1
		int bit = 1 << al;
		if(j->eobrun > 0)
		{
			j->eobrun--;
			for(k = ss; k <= se; k++)
			{
				int16_t* p = &blk[kZigzag[k]];
				if(*p != 0 && GetBit(b) && (*p & bit) == 0)
					*p = (int16_t)(*p > 0 ? *p + bit : *p - bit);
			}
			return 1;
		}
		k = ss;
		do
		{
			int rs = Decode(b, &j->ac[c->ta]), r, s;
			if(rs < 0)
				return 0;
			r = rs >> 4;
			s = rs & 15;
			if(s == 0)
			{
				if(r < 15)
				{
					j->eobrun = (1 << r) - 1;
					if(r)
						j->eobrun += (int)GetBits(b, r);
					r = 64; // refine the rest of the band, place nothing
				}
				// r == 15: sixteen zero coefficients
			}
			else
			{
				if(s != 1)
					return 0;
				s = GetBit(b) ? bit : -bit;
			}
			while(k <= se)
			{
				int16_t* p = &blk[kZigzag[k++]];
				if(*p != 0)
				{
					if(GetBit(b) && (*p & bit) == 0)
						*p = (int16_t)(*p > 0 ? *p + bit : *p - bit);
				}
				else
				{
					if(r == 0)
					{
						*p = (int16_t)s;
						break;
					}
					r--;
				}
			}
		} while(k <= se);
		return 1;
	}
}

// skip to the RSTn marker after an interval and reset the predictions
static void Restart(Jpeg_t* j, Bits_t* b)
{
	int i;
	while(b->p + 1 < b->end && !(b->p[0] == 0xFF && b->p[1] >= 0xD0 && b->p[1] <= 0xD7))
		b->p++;
	if(b->p + 1 < b->end)
		b->p += 2;
	b->buf = 0;
	b->n = 0;
	b->marker = 0;
	for(i = 0; i < j->ncomp; i++)
		j->comp[i].dcPred = 0;
	j->eobrun = 0;
}

// a scan whose data breaks off: what it decoded stays, and nothing after it is read
static size_t Damaged(Jpeg_t* j, size_t size)
{
	j->scans++;
	return size;
}

/* SOS: the scan's header in s, its entropy-coded data from `data`; the
 * offset of the marker after the data (the end of the file when the data
 * breaks off), or 0 when the scan's header is unusable */
static size_t ReadScan(Jpeg_t* j, const uint8_t* s, uint32_t len, const uint8_t* data, size_t pos, size_t size)
{
	Comp_t* sc[4];
	int ns, i, ss, se, ah, al, todo;
	Bits_t b;
	if(!j->frame || len < 1)
		return 0;
	ns = s[0];
	if(ns < 1 || ns > j->ncomp || len < 4u + 2u * (uint32_t)ns)
		return 0;
	for(i = 0; i < ns; i++)
	{
		int c, id = s[1 + 2 * i];
		for(c = 0; c < j->ncomp && j->comp[c].id != id; c++)
			;
		if(c == j->ncomp)
			return 0;
		sc[i] = &j->comp[c];
		sc[i]->td = s[2 + 2 * i] >> 4;
		sc[i]->ta = s[2 + 2 * i] & 15;
		if(sc[i]->td > 3 || sc[i]->ta > 3)
			return 0;
		sc[i]->dcPred = 0;
	}
	ss = s[1 + 2 * ns];
	se = s[2 + 2 * ns];
	ah = s[3 + 2 * ns] >> 4;
	al = s[3 + 2 * ns] & 15;
	if(!j->progressive)
		ss = 0, se = 63, ah = al = 0;
	else if(ss > se || se > 63 || (ss > 0 && ns != 1) || al > 13)
		return 0;
	for(i = 0; i < ns; i++)
	{
		int needDc = ss == 0 && ah == 0, needAc = !j->progressive || ss > 0;
		if((needDc && !j->dc[sc[i]->td].present) || (needAc && !j->ac[sc[i]->ta].present))
			return 0;
	}
	j->eobrun = 0;
	b.p = data + pos;
	b.end = data + size;
	b.buf = 0;
	b.n = 0;
	b.marker = 0;
	todo = j->restart;
	if(ns == 1)
	{ // one component: its blocks in raster order, each an MCU
		Comp_t* c = sc[0];
		int cw = (c->w + 7) / 8, ch = (c->ht + 7) / 8, x, y;
		for(y = 0; y < ch; y++)
			for(x = 0; x < cw; x++)
			{
				int16_t* blk = c->coef + ((size_t)y * (size_t)c->bw + (size_t)x) * 64;
				if(!(j->progressive ? BlockProg(j, &b, c, blk, ss, se, ah, al) : BlockSeq(j, &b, c, blk)))
					return Damaged(j, size);
				if(j->restart && --todo == 0 && !(y == ch - 1 && x == cw - 1))
				{
					Restart(j, &b);
					todo = j->restart;
				}
			}
	}
	else
	{
		int mx, my;
		for(my = 0; my < j->mcuy; my++)
			for(mx = 0; mx < j->mcux; mx++)
			{
				for(i = 0; i < ns; i++)
				{
					Comp_t* c = sc[i];
					int bx, by;
					for(by = 0; by < c->v; by++)
						for(bx = 0; bx < c->h; bx++)
						{
							size_t at = ((size_t)(my * c->v + by) * (size_t)c->bw + (size_t)(mx * c->h + bx)) * 64;
							if(!(j->progressive ? BlockProg(j, &b, c, c->coef + at, ss, se, ah, al)
												: BlockSeq(j, &b, c, c->coef + at)))
								return Damaged(j, size);
						}
				}
				if(j->restart && --todo == 0 && !(my == j->mcuy - 1 && mx == j->mcux - 1))
				{
					Restart(j, &b);
					todo = j->restart;
				}
			}
	}
	j->scans++;
	// the next marker (not a restart, not a stuffed byte)
	{
		const uint8_t* p = b.p;
		while(p + 1 < b.end && !(p[0] == 0xFF && p[1] != 0 && !(p[1] >= 0xD0 && p[1] <= 0xD7)))
			p++;
		return (size_t)(p - data);
	}
}

// dequantise and inverse-transform every block of a component into its samples
static void Reconstruct(Jpeg_t* j, Comp_t* c)
{
	const uint16_t* q = j->q[c->tq];
	int stride = c->bw * 8, bx, by;
	c->pix = (uint8_t*)BGI_Alloc((size_t)stride * (size_t)c->bh * 8);
	for(by = 0; by < c->bh; by++)
		for(bx = 0; bx < c->bw; bx++)
		{
			const int16_t* blk = c->coef + ((size_t)by * (size_t)c->bw + (size_t)bx) * 64;
			float f[64], t[64];
			int u, v, x, y;
			for(u = 0; u < 64; u++)
				f[u] = (float)blk[u] * (float)q[u];
			for(v = 0; v < 8; v++) // rows of frequencies into rows of samples
			{
				const float* fr = f + v * 8;
				for(x = 0; x < 8; x++)
				{
					float s = 0.0f;
					for(u = 0; u < 8; u++)
						s += kCos[x][u] * fr[u];
					t[v * 8 + x] = s;
				}
			}
			for(x = 0; x < 8; x++)
				for(y = 0; y < 8; y++)
				{
					float s = 0.0f;
					for(v = 0; v < 8; v++)
						s += kCos[y][v] * t[v * 8 + x];
					c->pix[(size_t)(by * 8 + y) * (size_t)stride + (size_t)(bx * 8 + x)] = (uint8_t)Clamp255(s + 128.0f);
				}
		}
}

/* the sample of component c for output pixel x, y: bilinear between the
 * component's samples, centred, the edges repeated */
typedef struct Axis
{
	int* i0; // the first sample
	int* i1; // the second
	int* f;  // the weight of the second, 0 .. 256
} Axis_t;

static void MakeAxis(Axis_t* a, int out, int in, int num, int den)
{
	int i;
	a->i0 = (int*)BGI_Alloc((size_t)out * sizeof(int));
	a->i1 = (int*)BGI_Alloc((size_t)out * sizeof(int));
	a->f = (int*)BGI_Alloc((size_t)out * sizeof(int));
	for(i = 0; i < out; i++)
	{
		double p = ((double)i + 0.5) * num / den - 0.5;
		int lo = (int)floor(p);
		int fr = (int)((p - lo) * 256.0 + 0.5);
		if(lo < 0)
			lo = 0, fr = 0;
		if(lo >= in - 1)
			lo = in - 1, fr = 0;
		a->i0[i] = lo;
		a->i1[i] = lo + 1 < in ? lo + 1 : lo;
		a->f[i] = fr;
	}
}

static void FreeAxis(Axis_t* a)
{
	BGI_Free(a->i0);
	BGI_Free(a->i1);
	BGI_Free(a->f);
}

// a component scaled to the image size, w * h samples
static uint8_t* Upsample(Jpeg_t* j, Comp_t* c)
{
	int w = j->width, h = j->height, stride = c->bw * 8, x, y;
	uint8_t* out = (uint8_t*)BGI_Alloc((size_t)w * (size_t)h);
	if(c->h == j->hmax && c->v == j->vmax)
	{
		for(y = 0; y < h; y++)
			memcpy(out + (size_t)y * (size_t)w, c->pix + (size_t)y * (size_t)stride, (size_t)w);
	}
	else
	{
		Axis_t ax, ay;
		MakeAxis(&ax, w, c->w, c->h, j->hmax);
		MakeAxis(&ay, h, c->ht, c->v, j->vmax);
		for(y = 0; y < h; y++)
		{
			const uint8_t* r0 = c->pix + (size_t)ay.i0[y] * (size_t)stride;
			const uint8_t* r1 = c->pix + (size_t)ay.i1[y] * (size_t)stride;
			int fy = ay.f[y];
			for(x = 0; x < w; x++)
			{
				int fx = ax.f[x];
				int a = r0[ax.i0[x]] * (256 - fx) + r0[ax.i1[x]] * fx;
				int b = r1[ax.i0[x]] * (256 - fx) + r1[ax.i1[x]] * fx;
				out[(size_t)y * (size_t)w + (size_t)x] = (uint8_t)((a * (256 - fy) + b * fy + 32768) >> 16);
			}
		}
		FreeAxis(&ax);
		FreeAxis(&ay);
	}
	return out;
}

static void FreeJpeg(Jpeg_t* j)
{
	int i;
	for(i = 0; i < 4; i++)
	{
		BGI_Free(j->comp[i].coef);
		BGI_Free(j->comp[i].pix);
	}
}

int Jpeg_Decode(const uint8_t* data, size_t size, Picture_t* out)
{
	Jpeg_t* j;
	size_t pos = 2;
	int ok = 1, i;
	memset(out, 0, sizeof *out);
	if(size < 4 || data[0] != 0xFF || data[1] != 0xD8)
		return 0;
	InitCos();
	j = (Jpeg_t*)BGI_Alloc(sizeof *j);
	memset(j, 0, sizeof *j);
	while(ok && pos + 2 <= size)
	{
		int m;
		uint32_t len;
		const uint8_t* s;
		if(data[pos] != 0xFF)
		{
			pos++; // stray bytes between segments
			continue;
		}
		m = data[pos + 1];
		if(m == 0xFF)
		{
			pos++; // fill bytes
			continue;
		}
		pos += 2;
		if(m == 0xD9)
			break;
		if((m >= 0xD0 && m <= 0xD7) || m == 0x01 || m == 0xD8)
			continue;
		if(pos + 2 > size)
			break;
		len = Be16(data + pos);
		if(len < 2 || pos + len > size)
			break;
		s = data + pos + 2;
		switch(m)
		{
			case 0xC0: // baseline
			case 0xC1: // extended, Huffman
			case 0xC2: // progressive, Huffman
				j->progressive = m == 0xC2;
				ok = ReadSof(j, s, len - 2);
				break;
			case 0xC3: // lossless, arithmetic coding, hierarchical: not read
			case 0xC5:
			case 0xC6:
			case 0xC7:
			case 0xC9:
			case 0xCA:
			case 0xCB:
			case 0xCD:
			case 0xCE:
			case 0xCF:
				ok = 0;
				break;
			case 0xC4:
				ok = ReadDht(j, s, len - 2);
				break;
			case 0xDB:
				ok = ReadDqt(j, s, len - 2);
				break;
			case 0xDD:
				j->restart = len >= 4 ? (int)Be16(s) : 0;
				break;
			case 0xEE: // Adobe: its transform flag says whether three components are YCbCr
				if(len >= 14 && memcmp(s, "Adobe", 5) == 0)
				{
					j->adobe = 1;
					j->adobeTransform = s[11];
				}
				break;
			case 0xDA:
			{
				size_t next = ReadScan(j, s, len - 2, data, pos + len, size);
				if(!next)
					ok = 0;
				pos = next;
				continue;
			}
			default:
				break;
		}
		pos += len;
	}
	if(!j->frame || j->scans == 0)
		ok = 0;
	if(ok)
	{
		size_t n = (size_t)j->width * (size_t)j->height, p;
		uint8_t* plane[3] = {NULL, NULL, NULL};
		for(i = 0; i < j->ncomp; i++)
		{
			Reconstruct(j, &j->comp[i]);
			plane[i] = Upsample(j, &j->comp[i]);
		}
		out->width = (uint32_t)j->width;
		out->height = (uint32_t)j->height;
		out->bgra = (uint8_t*)BGI_Alloc(n * 4);
		if(j->ncomp == 1)
		{ // grey: GDI+ gives an 8-bit picture with a grey palette
			out->kind = PIC_INDEXED8;
			out->index = plane[0];
			plane[0] = NULL;
			for(p = 0; p < n; p++)
			{
				uint8_t g = out->index[p];
				out->bgra[p * 4] = out->bgra[p * 4 + 1] = out->bgra[p * 4 + 2] = g;
				out->bgra[p * 4 + 3] = 0xFF;
			}
		}
		else
		{
			int rgb = (j->adobe && j->adobeTransform == 0) ||
				(j->comp[0].id == 'R' && j->comp[1].id == 'G' && j->comp[2].id == 'B');
			out->kind = PIC_RGB24;
			for(p = 0; p < n; p++)
			{
				uint8_t* d = out->bgra + p * 4;
				if(rgb)
				{
					d[0] = plane[2][p];
					d[1] = plane[1][p];
					d[2] = plane[0][p];
				}
				else
				{
					float y = plane[0][p], cb = plane[1][p] - 128.0f, cr = plane[2][p] - 128.0f;
					d[2] = (uint8_t)Clamp255(y + 1.402f * cr);
					d[1] = (uint8_t)Clamp255(y - 0.344136f * cb - 0.714136f * cr);
					d[0] = (uint8_t)Clamp255(y + 1.772f * cb);
				}
				d[3] = 0xFF;
			}
		}
		for(i = 0; i < 3; i++)
			BGI_Free(plane[i]);
	}
	FreeJpeg(j);
	BGI_Free(j);
	return ok;
}

// ---- encoder ---------------------------------------------------------------------

// the quantisation tables of the specification (annex K.1), natural order
static const uint8_t kStdLuma[64] = {16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24,
	40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
	49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
static const uint8_t kStdChroma[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99,
	99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// the Huffman tables of the specification (annex K.3): code counts by length, then the symbols
static const uint8_t kDcLumaBits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t kDcChromaBits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const uint8_t kDcVals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t kAcLumaBits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const uint8_t kAcLumaVals[162] = {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13,
	0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
	0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34,
	0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57,
	0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
	0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
	0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4,
	0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4,
	0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
static const uint8_t kAcChromaBits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
static const uint8_t kAcChromaVals[162] = {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51,
	0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
	0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29,
	0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56,
	0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
	0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
	0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2,
	0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3,
	0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

typedef struct ECode
{
	uint16_t code[256];
	uint8_t size[256];
} ECode_t;

typedef struct Out
{
	uint8_t* buf;
	size_t len, cap;
	uint32_t acc; // bits not yet written, at the bottom
	int n;
} Out_t;

static void Put(Out_t* out, uint8_t c)
{
	if(out->len == out->cap)
	{
		size_t cap = out->cap ? out->cap * 2 : 0x10000;
		uint8_t* nb = (uint8_t*)BGI_Alloc(cap);
		if(out->len)
			memcpy(nb, out->buf, out->len);
		BGI_Free(out->buf);
		out->buf = nb;
		out->cap = cap;
	}
	out->buf[out->len++] = c;
}

static void Put16(Out_t* out, uint32_t v)
{
	Put(out, (uint8_t)(v >> 8));
	Put(out, (uint8_t)v);
}

static void PutBits(Out_t* out, uint32_t v, int n)
{
	out->acc = (out->acc << n) | (v & ((1u << n) - 1u));
	out->n += n;
	while(out->n >= 8)
	{
		uint8_t c = (uint8_t)(out->acc >> (out->n - 8));
		Put(out, c);
		if(c == 0xFF)
			Put(out, 0); // stuffing
		out->n -= 8;
	}
}

static void MakeCodes(ECode_t* e, const uint8_t* bits, const uint8_t* vals)
{
	int l, i, k = 0;
	uint32_t code = 0;
	memset(e, 0, sizeof *e);
	for(l = 1; l <= 16; l++)
	{
		for(i = 0; i < bits[l - 1]; i++, k++)
		{
			e->code[vals[k]] = (uint16_t)code++;
			e->size[vals[k]] = (uint8_t)l;
		}
		code <<= 1;
	}
}

static void PutDht(Out_t* out, int cls, int id, const uint8_t* bits, const uint8_t* vals)
{
	int i, total = 0;
	for(i = 0; i < 16; i++)
		total += bits[i];
	Put16(out, 0xFFC4);
	Put16(out, (uint32_t)(2 + 1 + 16 + total));
	Put(out, (uint8_t)(cls << 4 | id));
	for(i = 0; i < 16; i++)
		Put(out, bits[i]);
	for(i = 0; i < total; i++)
		Put(out, vals[i]);
}

// the number of bits of a value's magnitude
static int Category(int v)
{
	int n = 0;
	if(v < 0)
		v = -v;
	while(v)
	{
		n++;
		v >>= 1;
	}
	return n;
}

// transform, quantise and code one block of samples (level-shifted by the caller's +128 convention)
static void EncodeBlock(Out_t* out, const float* s, const uint8_t* q, int* pred, const ECode_t* dc, const ECode_t* ac)
{
	float t[64];
	int c[64], u, v, x, y, k, run, cat, diff;
	for(y = 0; y < 8; y++) // rows: samples into frequencies
		for(u = 0; u < 8; u++)
		{
			float a = 0.0f;
			for(x = 0; x < 8; x++)
				a += kCos[x][u] * (s[y * 8 + x] - 128.0f);
			t[y * 8 + u] = a;
		}
	for(u = 0; u < 8; u++)
		for(v = 0; v < 8; v++)
		{
			float a = 0.0f;
			for(y = 0; y < 8; y++)
				a += kCos[y][v] * t[y * 8 + u];
			c[v * 8 + u] = (int)floorf(a / (float)q[v * 8 + u] + 0.5f);
		}
	diff = c[0] - *pred;
	*pred = c[0];
	cat = Category(diff);
	PutBits(out, dc->code[cat], dc->size[cat]);
	if(cat)
		PutBits(out, (uint32_t)(diff < 0 ? diff - 1 : diff), cat);
	run = 0;
	for(k = 1; k < 64; k++)
	{
		int a = c[kZigzag[k]];
		if(a == 0)
		{
			run++;
			continue;
		}
		while(run > 15)
		{
			PutBits(out, ac->code[0xF0], ac->size[0xF0]);
			run -= 16;
		}
		cat = Category(a);
		PutBits(out, ac->code[run << 4 | cat], ac->size[run << 4 | cat]);
		PutBits(out, (uint32_t)(a < 0 ? a - 1 : a), cat);
		run = 0;
	}
	if(run)
		PutBits(out, ac->code[0], ac->size[0]);
}

uint8_t* Jpeg_Encode(const uint8_t* bgrx, uint32_t w, uint32_t h, size_t pitch, int quality, size_t* size)
{
	Out_t out;
	uint8_t ql[64], qc[64];
	ECode_t dcl, dcc, acl, acc;
	int scale, i, predY = 0, predCb = 0, predCr = 0;
	uint32_t mx, my;
	memset(&out, 0, sizeof out);
	if(w == 0 || h == 0 || w > 0xFFFF || h > 0xFFFF)
		return NULL;
	InitCos();
	if(quality < 1)
		quality = 1;
	if(quality > 100)
		quality = 100;
	scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
	for(i = 0; i < 64; i++)
	{
		int a = (kStdLuma[i] * scale + 50) / 100, b = (kStdChroma[i] * scale + 50) / 100;
		ql[i] = (uint8_t)(a < 1 ? 1 : a > 255 ? 255
											  : a);
		qc[i] = (uint8_t)(b < 1 ? 1 : b > 255 ? 255
											  : b);
	}
	MakeCodes(&dcl, kDcLumaBits, kDcVals);
	MakeCodes(&dcc, kDcChromaBits, kDcVals);
	MakeCodes(&acl, kAcLumaBits, kAcLumaVals);
	MakeCodes(&acc, kAcChromaBits, kAcChromaVals);

	Put16(&out, 0xFFD8);
	Put16(&out, 0xFFE0); // JFIF 1.01, no density
	Put16(&out, 16);
	Put(&out, 'J');
	Put(&out, 'F');
	Put(&out, 'I');
	Put(&out, 'F');
	Put(&out, 0);
	Put16(&out, 0x0101);
	Put(&out, 0);
	Put16(&out, 1);
	Put16(&out, 1);
	Put16(&out, 0);
	Put16(&out, 0xFFDB);
	Put16(&out, 2 + 2 * 65);
	Put(&out, 0);
	for(i = 0; i < 64; i++)
		Put(&out, ql[kZigzag[i]]);
	Put(&out, 1);
	for(i = 0; i < 64; i++)
		Put(&out, qc[kZigzag[i]]);
	Put16(&out, 0xFFC0);
	Put16(&out, 8 + 3 * 3);
	Put(&out, 8);
	Put16(&out, h);
	Put16(&out, w);
	Put(&out, 3);
	Put(&out, 1);
	Put(&out, 0x22);
	Put(&out, 0);
	Put(&out, 2);
	Put(&out, 0x11);
	Put(&out, 1);
	Put(&out, 3);
	Put(&out, 0x11);
	Put(&out, 1);
	PutDht(&out, 0, 0, kDcLumaBits, kDcVals);
	PutDht(&out, 1, 0, kAcLumaBits, kAcLumaVals);
	PutDht(&out, 0, 1, kDcChromaBits, kDcVals);
	PutDht(&out, 1, 1, kAcChromaBits, kAcChromaVals);
	Put16(&out, 0xFFDA);
	Put16(&out, 6 + 2 * 3);
	Put(&out, 3);
	Put(&out, 1);
	Put(&out, 0x00);
	Put(&out, 2);
	Put(&out, 0x11);
	Put(&out, 3);
	Put(&out, 0x11);
	Put(&out, 0);
	Put(&out, 63);
	Put(&out, 0);

	for(my = 0; my < (h + 15) / 16; my++)
		for(mx = 0; mx < (w + 15) / 16; mx++)
		{
			float y[4][64], cb[64], cr[64];
			float fcb[16][16], fcr[16][16];
			int px, py, b;
			for(py = 0; py < 16; py++)
				for(px = 0; px < 16; px++)
				{
					uint32_t sx = mx * 16 + (uint32_t)px, sy = my * 16 + (uint32_t)py;
					const uint8_t* p;
					float r, g, bl;
					if(sx >= w)
						sx = w - 1; // the edges repeat
					if(sy >= h)
						sy = h - 1;
					p = bgrx + (size_t)sy * pitch + (size_t)sx * 4;
					bl = p[0], g = p[1], r = p[2];
					y[(py / 8) * 2 + px / 8][(py % 8) * 8 + px % 8] = 0.299f * r + 0.587f * g + 0.114f * bl;
					fcb[py][px] = -0.168736f * r - 0.331264f * g + 0.5f * bl + 128.0f;
					fcr[py][px] = 0.5f * r - 0.418688f * g - 0.081312f * bl + 128.0f;
				}
			for(py = 0; py < 8; py++)
				for(px = 0; px < 8; px++)
				{
					cb[py * 8 + px] = (fcb[py * 2][px * 2] + fcb[py * 2][px * 2 + 1] + fcb[py * 2 + 1][px * 2] +
										  fcb[py * 2 + 1][px * 2 + 1]) *
						0.25f;
					cr[py * 8 + px] = (fcr[py * 2][px * 2] + fcr[py * 2][px * 2 + 1] + fcr[py * 2 + 1][px * 2] +
										  fcr[py * 2 + 1][px * 2 + 1]) *
						0.25f;
				}
			for(b = 0; b < 4; b++)
				EncodeBlock(&out, y[b], ql, &predY, &dcl, &acl);
			EncodeBlock(&out, cb, qc, &predCb, &dcc, &acc);
			EncodeBlock(&out, cr, qc, &predCr, &dcc, &acc);
		}
	if(out.n)
		PutBits(&out, 0x7F, 8 - out.n); // pad with ones
	Put16(&out, 0xFFD9);
	*size = out.len;
	return out.buf;
}
