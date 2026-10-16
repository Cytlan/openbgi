/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pixbuf.h - PixBuf, the 12-byte "buffer that knows whether it was
 *            allocated".  Script threads use it for their stack and code
 *            copies, bitmap slots and display objects for their pixels.
 *            Implemented in src/vm/thread.c.
 *
 * A PixBuf holds at most one allocation at a time: PixBuf_Alloc refuses a
 * second one until PixBuf_Free has released the first.
 */
#ifndef BGI_PIXBUF_H_
#define BGI_PIXBUF_H_

#include "bgi/common.h"

typedef struct PixBuf
{
	int allocated; // 1 while `data` holds an allocation
	uint8_t* data; // the bytes (BGI_Alloc, uninitialised), NULL when not allocated
	uint32_t size; // bytes allocated, 0 when not allocated
} PixBuf_t;
void PixBuf_Ctor(PixBuf_t* b);
void PixBuf_Dtor(PixBuf_t* b);                  // = PixBuf_Free
uint8_t* PixBuf_Alloc(PixBuf_t* b, uint32_t n); // n uninitialised bytes; NULL if already allocated
int PixBuf_Free(PixBuf_t* b);                   // release; the previous `allocated`

#endif // BGI_PIXBUF_H_
