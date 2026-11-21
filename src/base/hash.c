/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * hash.c - the engine's pseudo-random generator ("hash"); interface in
 *          codec.h
 *
 * Two entry points on the same step function: BGI_HashUpdate, here, on a
 * state the caller owns (the CompressedBG decoder); BGI_HashSeed /
 * BGI_HashNext (src/core/sysutil.c) on the engine's own state (DSC, the
 * save scrambler, the SDC and DCFS encoders).  Every
 * obfuscated file is "decrypted" by subtracting successive outputs from its
 * bytes, so a decoder seeds the generator with the key stored in the file
 * and steps it once per byte.
 *
 * The step is a 16/16-bit lagged multiplicative generator:
 *
 *      lo  = 20021 * (key & 0xffff)
 *      hi  = 20021 * (key >> 16) + 346 * key
 *      hi  = (hi + (lo >> 16)) & 0xffff
 *      key = (hi << 16) + (lo & 0xffff) + 1
 *      out = hi & 0x7fff
 *
 * A note on the global version: the original reads the high half of the key
 * with a 32-bit load two bytes into the key, which also picks up the first
 * two bytes of the string that happens to follow it in memory ("SDC FORMAT
 * 1.00").  Because the extra bits only ever land above bit 15 and every
 * result is masked to 16 bits, they have no effect on the output, so the
 * clean formulation above is exactly equivalent.
 */
#include "bgi/codec.h"

// one step on *key; returns the next 15-bit output
uint32_t BGI_HashUpdate(uint32_t* key)
{
	uint32_t k = *key;
	uint32_t lo = 20021u * (k & 0xffffu);
	uint32_t hi = 20021u * (k >> 16) + 346u * k;
	hi = (hi + (lo >> 16)) & 0xffffu;
	*key = (hi << 16) + (lo & 0xffffu) + 1u;
	return hi & 0x7fffu;
}
