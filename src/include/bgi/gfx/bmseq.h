/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bmseq.h - the registry of loaded BF_Movie sequences behind "90 F4 .. F7"
 *           (1.69/472 on; src/gfx/bmseq.c)
 *
 * A loaded movie is a record on a list with a number from a counter; a
 * record may have one "clone" sharing its data under a second number
 * ("90 F7"), and the data is released with the last of the two.  The data
 * is the whole file, or for a streamed movie loaded as one ("92 F1") its
 * head only: the frames are then read from the file as they are decoded.
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
uint32_t BmSeq_Register(OpenBGI_t* o, const void* data, uint32_t size, int32_t* outNo, int32_t info[5]);
/* "92 F1" with its flag set: a streamed movie that stays in its file.  `head`
 * is the file's first headSize bytes (BfMovie_StreamedHeadSize); `arc`
 * (NULL: a loose file) and `name` say where the frames are read from when
 * they are decoded, fileSize how long the file is.  0 ok, 0x80000002 not
 * the head of a streamed movie. */
uint32_t BmSeq_RegisterStreamed(OpenBGI_t* o, const void* head, uint32_t headSize, const char* arc, const char* name, uint32_t fileSize,
	int32_t* outNo, int32_t info[5]);
uint32_t BmSeq_Free(OpenBGI_t* o, int32_t no);                              // "90 F5": drop the record; the data goes when its clone (if any) is gone too
uint32_t BmSeq_Clone(OpenBGI_t* o, int32_t no, int32_t* outNo);             // "90 F7": a second number for the same data (one clone per record)
uint32_t BmSeq_Decode(OpenBGI_t* o, int bmpNo, int32_t no, uint32_t frame); // "90 F6": frame `frame` into bitmap slot bmpNo (same size and mode)
void BmSeq_FreeAll(OpenBGI_t* o);                                           // drop every record

/* "80 53": the next "90 F6" decodes inside a wait object (the
 * original's decoder pool) instead of at once; the flag is consumed by it */
void BmSeq_SetDeferFlag(OpenBGI_t* o, int on);
int BmSeq_TakeDeferFlag(OpenBGI_t* o);

#endif // BGI_BMSEQ_H_
