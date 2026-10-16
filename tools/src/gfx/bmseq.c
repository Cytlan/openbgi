/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bmseq.c - BF_Movie sequence records (inc/bgi/gfx/bmseq.h): the list of
 * loaded movies behind "90 F4 .. F7", their clones and the frame decode
 * into a bitmap slot
 *
 * The original keeps a critical section per record and one for the list
 * because the decoder pool and the loader thread touch them; here every
 * call runs on the engine thread.
 */
#include "bgi/gfx/bmseq.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/bfmovie.h"

typedef struct BmSeq
{
	int32_t no;         // the number the scripts use (from gCounter)
	uint8_t* data;      // a copy of the file (shared with the clone)
	uint32_t size;      // bytes of data
	int32_t cloneOf;    // the record this one was cloned from (0: none)
	int32_t clone;      // the clone made of this one (0: none)
	struct BmSeq* next; // the list, newest first
} BmSeq_t;

static BmSeq_t* gList;   // every loaded movie
static int32_t gCounter; // the last number handed out (numbers start at 1)
static int gDeferFlag;   // "80 53": the next decode goes through a wait object

// the record with number `no`, NULL when there is none
static BmSeq_t* Find(int32_t no)
{
	BmSeq_t* s;
	for(s = gList; s; s = s->next)
		if(s->no == no)
			return s;
	return NULL;
}

/* Register a copy of a BF_Movie file of `size` bytes under a new number
 * (*outNo); info[] receives width, height, pixel mode, frame rate and
 * frame count from its header.  0 ok, 0x80000002 not a movie. */
uint32_t BmSeq_Register(const void* data, uint32_t size, int32_t* outNo, int32_t info[5])
{
	const BfMovieHeader_t* h = (const BfMovieHeader_t*)data;
	BmSeq_t* s;
	if(!BfMovie_Check(data, size))
		return 0x80000002u;
	s = (BmSeq_t*)BGI_Alloc(sizeof *s);
	memset(s, 0, sizeof *s);
	s->no = ++gCounter;
	s->data = (uint8_t*)BGI_Alloc(size);
	memcpy(s->data, data, size);
	s->size = size;
	s->next = gList;
	gList = s;
	*outNo = s->no;
	info[0] = (int32_t)h->width;
	info[1] = (int32_t)h->height;
	info[2] = (int32_t)h->mode;
	info[3] = (int32_t)h->fps;
	info[4] = (int32_t)h->frames;
	return 0;
}

/* Drop record `no`.  Its partner (original or clone) is unlinked and the
 * file data is freed with the last of the two.  0 ok, 0x80000003 no such
 * number. */
uint32_t BmSeq_Free(int32_t no)
{
	BmSeq_t *s, *prev = NULL, *o;
	for(s = gList; s && s->no != no; s = s->next)
		prev = s;
	if(!s)
		return 0x80000003u;
	if(prev)
		prev->next = s->next;
	else
		gList = s->next;
	// unlink the partner; the data goes with the last of the pair
	if(s->cloneOf && (o = Find(s->cloneOf)) != NULL)
		o->clone = 0;
	if(s->clone && (o = Find(s->clone)) != NULL)
		o->cloneOf = 0;
	if(!s->cloneOf && !s->clone)
		BGI_Free(s->data);
	BGI_Free(s);
	return 0;
}

/* Give record `no` a second number (*outNo) that shares its data.  0 ok,
 * 0x80000003 no such number, 0x80000007 it already has a clone. */
uint32_t BmSeq_Clone(int32_t no, int32_t* outNo)
{
	BmSeq_t *s = Find(no), *c;
	if(!s)
		return 0x80000003u;
	if(s->clone)
		return 0x80000007u;
	c = (BmSeq_t*)BGI_Alloc(sizeof *c);
	memset(c, 0, sizeof *c);
	c->no = ++gCounter;
	c->data = s->data;
	c->size = s->size;
	c->cloneOf = no;
	c->next = gList;
	gList = c;
	s->clone = c->no;
	*outNo = c->no;
	return 0;
}

/* Decode frame `frame` of movie `no` into bitmap slot bmpNo, which must
 * hold a 32-bit bitmap of the movie's width, height and pixel mode.  0 ok,
 * 0x80000003 no such number, 0x80000004 no such frame, 0x80000005 the
 * slot does not match, 0x8000000A a streamed movie type this build does
 * not decode (reported once on stderr), 0x80000008 the decoder failed. */
uint32_t BmSeq_Decode(int bmpNo, int32_t no, uint32_t frame)
{
	BmSeq_t* s = Find(no);
	const BfMovieHeader_t* h;
	Bmp_t b;
	if(!s)
		return 0x80000003u;
	h = (const BfMovieHeader_t*)s->data;
	if(frame >= h->frames)
		return 0x80000004u;
	if(!BmpMgr_GetInfo(gBmpMgr, &b, bmpNo) || (uint32_t)b.w != h->width || (uint32_t)b.h != h->height ||
		(uint32_t)b.mode != h->mode || b.bpp != 4)
		return 0x80000005u;
	if(!BfMovie_IsSimple(s->data))
	{
		static int warned;
		if(!warned)
		{
			fprintf(stderr, "bgi: BF_Movie type 0x%x (the streamed form) is not decoded by this build\n",
				(unsigned)h->type);
			warned = 1;
		}
		return 0x8000000au;
	}
	if(!BfMovie_DecodeFrame(s->data, s->size, frame, b.pixels, b.pitch))
		return 0x80000008u; // (the pool's "decode failed")
	return 0;
}

// drop every record (engine shutdown)
void BmSeq_FreeAll(void)
{
	while(gList)
		BmSeq_Free(gList->no);
}

void BmSeq_SetDeferFlag(int on)
{
	gDeferFlag = on;
}

// read and clear the flag
int BmSeq_TakeDeferFlag(void)
{
	int f = gDeferFlag;
	gDeferFlag = 0;
	return f;
}
