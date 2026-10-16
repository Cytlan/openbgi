/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bmseq.h - the registry of loaded BF_Movie sequences behind "90 F4 .. F7"
 *           (1.69/472 on; src/gfx/bmseq.c)
 *
 * A loaded movie is a record on a list with a number from a global
 * counter; a record may have one "clone" sharing its data under a second
 * number ("90 F7"), and the data is released with the last of the two.
 * "90 F6" decodes a frame straight into a bitmap slot of the movie's size
 * and pixel mode.
 *
 * Result codes are the original's: 0 ok, 0x80000002 not a movie,
 * 0x80000003 no such number, 0x80000004 no such frame, 0x80000005 the
 * slot does not match, 0x80000007 already cloned, 0x80000008 the frame
 * could not be decoded, 0x8000000A a type this build cannot decode.
 */
#ifndef BGI_BMSEQ_H_
#define BGI_BMSEQ_H_

#include "bgi/common.h"

// "90 F4": register a copy of the file; *outNo receives its number, `info` width, height, pixel mode, frame rate, frame count
uint32_t BmSeq_Register(const void* data, uint32_t size, int32_t* outNo, int32_t info[5]);
uint32_t BmSeq_Free(int32_t no);                              // "90 F5": drop the record; the data goes when its clone (if any) is gone too
uint32_t BmSeq_Clone(int32_t no, int32_t* outNo);             // "90 F7": a second number for the same data (one clone per record)
uint32_t BmSeq_Decode(int bmpNo, int32_t no, uint32_t frame); // "90 F6": frame `frame` into bitmap slot bmpNo (same size and mode)
void BmSeq_FreeAll(void);                                     // drop every record

/* "80 53": the next "90 F6" decodes inside a wait object (the
 * original's decoder pool) instead of at once; the flag is consumed by it */
void BmSeq_SetDeferFlag(int on);
int BmSeq_TakeDeferFlag(void);

#endif // BGI_BMSEQ_H_
