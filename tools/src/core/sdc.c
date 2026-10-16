/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sdc.c - the save-data codecs; interface in codec.h
 *
 *   SDC FORMAT 1.00   a byte-oriented LZ77 coder whose output is scrambled
 *                     with the engine's "hash" generator.  Used for BGI.gdb
 *                     (sysobj/database.c) and by the "80 C0" (encode, on a
 *                     worker thread) / "80 C1" (decode) instructions.
 *   DCFS FORMAT 1.00  "difference-coded frames": a sequence of equally sized
 *                     frames where every frame after the first is stored as
 *                     alternating runs of "same as previous frame" / "literal
 *                     bytes", run lengths as base-128 varints.  Used by
 *                     "80 C4" / "80 C5", which wrap the DCFS data in an SDC
 *                     layer.
 *
 * SDC file layout (header 0x20 bytes, little endian, byte offsets):
 *      0x00  "SDC FORMAT 1.00\0"
 *      0x10  key      16-bit seed (the millisecond field of the system time
 *                     when encoding)
 *      0x14  encLen   bytes of LZ data following the header
 *      0x18  outSize  size of the original data
 *      0x1c  sum      16-bit sum of the scrambled payload bytes
 *      0x1e  xor      16-bit xor of the scrambled payload bytes
 *      0x20  payload: lz[i] + (hash_next() & 0xff), one generator step per byte
 *
 * LZ tokens:  0xxxxxxx            literal run of (x + 1) bytes follows
 *             1LLLLDDD dddddddd   copy (L + 2) bytes from (D:d + 2) back
 * window 0x801 bytes, match length 2..17.
 *
 * Both encoders verify their output by decoding it again before reporting
 * success.  The scrambler uses the global hash generator and takes the
 * engine lock while it does.
 */
#include "bgi/codec.h"
#include "bgi/sys.h"
#include "bgi/os.h"

#define SDC_WINDOW  0x801 // bytes the LZ coder looks back
#define SDC_MAX_LEN 0x11  // longest match a token can code

// -------------------------------------------------------------------------
// LZ encoder
// -------------------------------------------------------------------------

/* hash chains by first byte: a doubly linked ring per byte value, newest
 * first, over a ring of SDC_WINDOW node slots */
typedef struct LzNode
{
	uint32_t pos;        // position in the input
	struct LzNode* next; // towards older entries
	struct LzNode* prev; // towards newer entries / the head
} LzNode_t;

typedef struct LzState
{
	LzNode_t head[256];        // one sentinel per byte value (pos unused)
	LzNode_t ring[SDC_WINDOW]; // slot = pos % SDC_WINDOW
} LzState_t;

/* insert position `pos` (byte src[pos]) at the head of its chain, after
 * evicting the position that falls out of the window */
static void LzInsert(LzState_t* st, const uint8_t* src, uint32_t pos)
{
	LzNode_t *head, *n;
	if(pos >= SDC_WINDOW)
	{
		// the slot about to be reused is the oldest entry of the chain of the
		// byte that was at pos - SDC_WINDOW
		LzNode_t* h = &st->head[src[pos - SDC_WINDOW]];
		LzNode_t* oldest = h->prev;
		h->prev = oldest->prev;
		oldest->prev->next = h;
	}
	head = &st->head[src[pos]];
	n = &st->ring[pos % SDC_WINDOW];
	n->pos = pos;
	n->next = head->next;
	n->prev = head;
	head->next->prev = n;
	head->next = n;
}

/* LZ-code `size` bytes of src into dst (greedy longest match per position);
 * returns the number of bytes written */
static uint32_t SdcLzEncode(uint8_t* dst, const uint8_t* src, uint32_t size)
{
	LzState_t* st = (LzState_t*)BGI_Alloc(sizeof(LzState_t));
	uint8_t* out = dst;
	uint32_t total = 0;    // bytes written
	uint32_t litCount = 0; // pending literal bytes
	uint32_t litStart = 0; // their position in src
	uint32_t pos = 0;
	int i;

	for(i = 0; i < 256; i++)
	{
		st->head[i].next = &st->head[i];
		st->head[i].prev = &st->head[i];
	}

	while(pos < size)
	{
		uint32_t remaining = size - pos;
		uint32_t best = 0, bestDist = 0;
		LzNode_t* head = &st->head[src[pos]];
		LzNode_t* n;

		// longest match among the chain entries, newest first; a distance of
		// 1 cannot be coded (the token stores distance - 2)
		for(n = head->next; n != head; n = n->next)
		{
			uint32_t dist = pos - n->pos;
			uint32_t maxLen = remaining < SDC_MAX_LEN ? remaining : SDC_MAX_LEN;
			uint32_t len = 1;
			if(dist == 1 || maxLen <= 1)
				continue;
			while(len < maxLen && src[n->pos + len] == src[pos + len])
				len++;
			if(len <= 1)
				continue;
			if(len > best)
			{
				best = len;
				bestDist = dist;
			}
			if(len == maxLen)
				break;
		}

		if(best == 0)
		{ // literal
			LzInsert(st, src, pos);
			pos++;
			litCount++;
		}

		// flush the literal run when it is full or a match follows it
		if(litCount >= 0x80 || (best > 1 && litCount > 0))
		{
			*out++ = (uint8_t)(litCount - 1);
			memcpy(out, src + litStart, litCount);
			out += litCount;
			total += litCount + 1;
			litCount = 0;
			litStart = pos;
		}

		if(best > 1)
		{ // match token
			uint32_t k, v;
			for(k = 0; k < best; k++)
				LzInsert(st, src, pos++);
			litStart = pos;
			v = (best << 11) + bestDist + 0x6ffe; // 0x8000 | (best-2)<<11 | (dist-2)
			*out++ = (uint8_t)(v >> 8);
			*out++ = (uint8_t)v;
			total += 2;
		}
	}

	if(litCount > 0)
	{
		*out++ = (uint8_t)(litCount - 1);
		memcpy(out, src + litStart, litCount);
		total += litCount + 1;
	}
	BGI_Free(st);
	return total;
}

// -------------------------------------------------------------------------
// LZ decoder
// -------------------------------------------------------------------------

// expand encLen bytes of LZ tokens into dst; returns the number of bytes produced
static uint32_t SdcLzDecode(uint8_t* dst, const uint8_t* lz, uint32_t encLen)
{
	uint32_t outPos = 0;
	while(encLen > 0)
	{
		uint8_t b = lz[0];
		if(b & 0x80)
		{
			uint32_t len = ((b >> 3) & 0xf) + 2;
			uint32_t dist = (((uint32_t)(b & 7) << 8) | lz[1]) + 2;
			uint32_t k;
			for(k = 0; k < len; k++, outPos++) // byte-wise: the copy may overlap itself
				dst[outPos] = dst[outPos - dist];
			lz += 2;
			encLen -= 2;
		}
		else
		{
			uint32_t n = (uint32_t)b + 1;
			memcpy(dst + outPos, lz + 1, n);
			outPos += n;
			lz += 1 + n;
			encLen -= 1 + n; // unsigned: a short stream ends the loop
		}
	}
	return outPos;
}

// -------------------------------------------------------------------------
// scrambling
// -------------------------------------------------------------------------

/* scramble the payload of the SDC file at `file` in place and fill key /
 * sum / xor of its header; encLen must already be set.  The key is the
 * millisecond field of the current system time. */
static void SdcScramble(uint8_t* file)
{
	SdcHeader_t* h = (SdcHeader_t*)file;
	uint8_t* p = file + sizeof(SdcHeader_t);
	uint32_t n = h->encLen, sum = 0, x = 0;
	OsLocalTime_t lt;

	EngineLock_Enter(); // the generator state is global
	// only the millisecond field of the time is used, which is the same in
	// local time and in the system time the original reads
	OS_LocalTime(&lt);
	BGI_HashSeed(lt.millis);
	while(n--)
	{
		uint8_t v = (uint8_t)(*p + BGI_HashNext());
		*p++ = v;
		sum += v;
		x ^= v;
	}
	h->key = lt.millis;
	h->sum = (uint16_t)sum;
	h->xor = (uint16_t)x;
	EngineLock_Leave();
}

// unscramble the payload into a new buffer (BGI_Free it); NULL when the checks fail
static uint8_t* SdcUnscramble(const uint8_t* file)
{
	const SdcHeader_t* h = (const SdcHeader_t*)file;
	const uint8_t* p = file + sizeof(SdcHeader_t);
	uint8_t* buf;
	uint32_t n = h->encLen, i, sum = 0, x = 0;

	EngineLock_Enter();
	BGI_HashSeed(h->key);
	buf = (uint8_t*)BGI_Alloc(n ? n : 1);
	for(i = 0; i < n; i++)
	{
		uint8_t v = p[i];
		buf[i] = (uint8_t)(v - BGI_HashNext());
		sum += v; // the checks cover the scrambled bytes
		x ^= v;
	}
	if((uint16_t)sum != h->sum || (uint16_t)x != h->xor)
	{
		BGI_Free(buf);
		buf = NULL;
	}
	EngineLock_Leave();
	return buf;
}

// -------------------------------------------------------------------------
// public SDC entry points
// -------------------------------------------------------------------------

int SdcIsFormat(const void* data)
{
	return memcmp(data, SDC_MAGIC, 16) == 0;
}

/* "80 C1": decode the SDC file at src into dst (SdcOutSize(src) bytes);
 * returns the decoded size, 0 when the magic or the checks fail */
uint32_t SdcDecode(void* dst, const void* src)
{
	const SdcHeader_t* h = (const SdcHeader_t*)src;
	uint8_t* lz;
	uint32_t n;
	if(!SdcIsFormat(src))
		return 0;
	lz = SdcUnscramble((const uint8_t*)src);
	if(!lz)
		return 0;
	n = SdcLzDecode((uint8_t*)dst, lz, h->encLen);
	BGI_Free(lz);
	return n;
}

/* "80 C0": encode size bytes of src into dst (header + payload).  The
 * result is verified by decoding it again; returns the total number of
 * bytes written, or 0 when the round trip failed. */
uint32_t SdcEncode(void* dst, const void* src, uint32_t size)
{
	uint8_t* copy = (uint8_t*)BGI_Alloc(size ? size : 1);
	uint8_t* file = (uint8_t*)dst;
	SdcHeader_t* h = (SdcHeader_t*)dst;
	uint8_t* check;
	uint32_t encLen, result = 0;

	// a private copy of the input; the verification compares against it
	memcpy(copy, src, size);
	encLen = SdcLzEncode(file + sizeof(SdcHeader_t), copy, size);
	memcpy(h->magic, SDC_MAGIC, 16);
	h->encLen = encLen;
	h->outSize = size;
	SdcScramble(file);

	check = (uint8_t*)BGI_Alloc(size ? size * 2 : 1);
	if(SdcDecode(check, dst) == size && memcmp(check, copy, size) == 0)
		result = encLen + sizeof(SdcHeader_t);
	BGI_Free(check);
	BGI_Free(copy);
	return result;
}

// -------------------------------------------------------------------------
// DCFS
// -------------------------------------------------------------------------

/* write v as a base-128 varint, low group first, 0x80 = more follows;
 * returns the bytes written */
static uint32_t VarintPut(uint8_t* dst, uint32_t v)
{
	uint32_t n = 0;
	do
	{
		uint8_t c = (uint8_t)(v & 0x7f);
		if(v >= 0x80)
			c |= 0x80;
		dst[n++] = c;
		v >>= 7;
	} while(v != 0);
	return n;
}

// read a varint at p into *out; returns the number of bytes consumed
static uint32_t VarintGet(uint32_t* out, const uint8_t* p)
{
	uint32_t v = 0, shift = 0, n = 0;
	uint8_t c;
	do
	{
		c = p[n++];
		v |= (uint32_t)(c & 0x7f) << shift;
		shift += 7;
	} while(c & 0x80);
	*out = v;
	return n;
}

/* "80 C5" (after the SDC layer): expand the DCFS file at src into dst
 * (frameSize * frameCount bytes).  0 ok, 0x80000003 bad magic, 0x80000004
 * when the frame size or count is zero or a frame's runs do not add up to
 * frameSize. */
uint32_t DcfsDecode(void* dst, const void* src)
{
	const DcfsHeader_t* h = (const DcfsHeader_t*)src;
	const uint8_t* p;
	uint8_t *prev, *cur;
	uint32_t frameSize, frameCount, f;

	if(memcmp(h->magic, DCFS_MAGIC, 16) != 0)
		return 0x80000003u;
	frameSize = h->frameSize;
	frameCount = h->frameCount;
	if(frameSize == 0 || frameCount == 0)
		return 0x80000004u;

	p = (const uint8_t*)src + sizeof(DcfsHeader_t);
	prev = (uint8_t*)dst;
	memcpy(prev, p, frameSize); // the first frame is stored raw
	p += frameSize;
	cur = prev + frameSize;

	for(f = 1; f < frameCount; f++)
	{
		uint32_t pos = 0, literal = 0; // the runs alternate, a copy run first
		while(pos < frameSize)
		{
			uint32_t run;
			p += VarintGet(&run, p);
			if(literal)
			{
				memcpy(cur, p, run);
				p += run;
			}
			else
			{
				memcpy(cur, prev, run);
			}
			cur += run;
			prev += run;
			pos += run;
			literal ^= 1;
		}
		if(pos != frameSize)
			return 0x80000004u;
	}
	return 0;
}

/* "80 C4" (before the SDC layer): encode frameCount frames of frameSize
 * bytes from src into dst.  *outSize receives the encoded length.  0 ok,
 * 0xffffffff when the verification decode differs (outSize is left alone),
 * 0x80000001 / 0x80000002 for a zero frame count / size. */
uint32_t DcfsEncode(void* dst, uint32_t* outSize, const void* src,
	uint32_t frameSize, uint32_t frameCount)
{
	DcfsHeader_t* h = (DcfsHeader_t*)dst;
	uint8_t* out;
	const uint8_t *prev, *cur;
	uint32_t total, f, result = 0;
	uint8_t* check;

	if(frameCount == 0)
		return 0x80000001u;
	if(frameSize == 0)
		return 0x80000002u;

	memcpy(h->magic, DCFS_MAGIC, 16);
	h->frameSize = frameSize;
	h->frameCount = frameCount;
	memcpy((uint8_t*)dst + sizeof(DcfsHeader_t), src, frameSize); // the first frame raw
	total = sizeof(DcfsHeader_t) + frameSize;
	out = (uint8_t*)dst + total;
	prev = (const uint8_t*)src;
	cur = prev + frameSize;

	for(f = 1; f < frameCount; f++)
	{
		uint32_t remaining = frameSize, literal = 0;
		while(remaining > 0)
		{
			uint32_t run = 0, n;
			// length of the next run: equal bytes for a copy run, differing
			// bytes for a literal run (0 when the first byte disagrees)
			if((literal == 0) == (cur[0] == prev[0]))
			{
				run = 1;
				while(run < remaining && (literal == 0) == (cur[run] == prev[run]))
					run++;
			}
			n = VarintPut(out, run);
			out += n;
			total += n;
			if(literal)
			{
				memcpy(out, cur, run);
				out += run;
				total += run;
			}
			prev += run;
			cur += run;
			remaining -= run;
			literal ^= 1;
		}
	}

	// verify by decoding
	check = (uint8_t*)BGI_Alloc(frameSize * frameCount);
	DcfsDecode(check, dst);
	if(memcmp(check, src, frameSize * frameCount) != 0)
		result = 0xffffffffu;
	else
		*outSize = total;
	BGI_Free(check);
	return result;
}
