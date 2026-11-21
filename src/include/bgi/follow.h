/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * follow.h - what a game makes of the engine's own program file
 *
 * A game can ask the engine for the path of the program it runs in
 * ("81 3F"), read that file and compare a number it computes from it with
 * one its programs carry: a check that the engine is the one they were
 * made for.  Here the engine is another one by nature, so the check is
 * answered the way the product name is: what a program computes from the
 * engine's file equals what it is compared with.
 *
 * The engine hands out a file of its own (file.h, ProgramPath), and what a
 * program gets from it is marked and followed:
 *
 *   - the bytes a loader instruction puts into a program's buffer, the
 *     size it tells and a digest of "80 E9" are marked;
 *   - the mark travels with a value: through the arithmetic and the bit
 *     operations, into the 32-bit variable a marked value is stored in and
 *     out of it again, through a copy of marked bytes ("60"), and with a
 *     value read through a marked pointer (a table looked up by it);
 *   - "30" and "31" find a marked value equal to an unmarked one, unless
 *     the unmarked one is 0 or -1 (a program's test for "no file");
 *     "63" finds marked bytes equal to unmarked ones.
 *
 * Nothing else changes: two marked values compare as they are, and so do
 * the ordered comparisons.  A check that does not end in one of these
 * comparisons is not answered.
 *
 * A value on the evaluation stack has its mark beside it (Thread_t,
 * vm.h).  The marks of memory are kept by address with the content that
 * was marked, and hold only while the memory still has that content, so
 * what an engine service writes there afterwards is never taken for
 * marked.
 */
#ifndef BGI_FOLLOW_H_
#define BGI_FOLLOW_H_

#include "bgi/common.h"

#define FOLLOW_BUCKETS 64 // 32-bit variables holding a marked value: buckets of FOLLOW_WAYS
#define FOLLOW_WAYS    4
#define FOLLOW_RUNS    8  // runs of marked bytes
#define FOLLOW_OWN     32 // the longest run that is not part of the program file (a digest)
#define FOLLOW_KEPT    8  // variables of the global memory remembered from one run to the next

typedef struct FollowCell
{
	uint8_t* addr;  // NULL: free
	uint32_t value; // what was stored there
} FollowCell_t;

typedef struct FollowRun
{
	uint8_t* addr;           // NULL: free
	uint32_t len;            // bytes
	const uint8_t* ref;      // what they were: part of the program file's image, or `own`
	uint8_t own[FOLLOW_OWN]; // the bytes of a run that is not part of the image
} FollowRun_t;

typedef struct FollowKept
{
	uint32_t off;   // the variable's offset in the global memory
	uint32_t value; // the marked value it held
} FollowKept_t;

typedef struct Follow
{
	uint32_t live; // cells, runs and kept variables in use: 0 while there is nothing to look up
	FollowCell_t cells[FOLLOW_BUCKETS][FOLLOW_WAYS];
	FollowRun_t runs[FOLLOW_RUNS];
	uint32_t nextRun; // the run replaced next when all are in use
	FollowKept_t kept[FOLLOW_KEPT];
	uint32_t keptCount;
} Follow_t;

/* [p, p + len) now holds `len` bytes that come from the program file:
 * `ref` points at them where they are part of the image (it is kept), NULL
 * says they are a result of their own, at most FOLLOW_OWN bytes, as they
 * stand at p. */
void Follow_Bytes(OpenBGI_t* o, uint8_t* p, uint32_t len, const uint8_t* ref);
// whether the `n` bytes (1, 2 or 4) a load read at p, the value v, are marked
int Follow_Loaded(OpenBGI_t* o, const uint8_t* p, uint32_t n, uint32_t v);
// a store of `n` bytes (1, 2 or 4) of v at p, marked or not
void Follow_Stored(OpenBGI_t* o, uint8_t* p, uint32_t n, uint32_t v, int marked);
// whether [p, p + len) has marked bytes
int Follow_Has(OpenBGI_t* o, const uint8_t* p, uint32_t len);
// `len` bytes were copied from src to dst: the marks go along
void Follow_Copied(OpenBGI_t* o, uint8_t* dst, const uint8_t* src, uint32_t len);
/* From one run to the next.  A game's save files hold the global memory,
 * and a marked value in a variable there is saved as the number it is: a
 * later run that loads the save has not computed it, and would compare it
 * as it is.  So where those variables are and what they held is kept
 * beside the game (".openbgi/follow.txt" in its folder, a line "global
 * <offset> <value>" each, in hexadecimal): Follow_Keep notes them - it is
 * called where the global memory is written out and at the end of a run -
 * and Follow_Start reads the note when a run begins.  A load of such a
 * variable that reads the value kept for it is marked. */
void Follow_Start(OpenBGI_t* o);
void Follow_Keep(OpenBGI_t* o);
/* `size` bytes at base are freed or moved (NULL: any memory a program can
 * address may be): nothing there is marked any more.  A run is compared
 * with its memory, so this is called wherever such memory goes. */
void Follow_Released(OpenBGI_t* o, const uint8_t* base, size_t size);

#endif
