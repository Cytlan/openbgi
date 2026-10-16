/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * deflate.h - the zlib stream format (RFC 1950 / 1951) and CRC-32, for the
 *             PNG files of the tools (png.h); not part of the original
 *             engine.  Implemented in src/core/deflate.c.
 *
 * A compressor and a decompressor of whole buffers, written for this
 * project so that the tools need no library: the compressor is a hash
 * chain LZ77 with lazy matching and one dynamic Huffman code per block
 * (a stored block where that is smaller), the decompressor takes every
 * block type.  Both work on memory; the results are malloc'ed.
 */
#ifndef BGI_DEFLATE_H_
#define BGI_DEFLATE_H_

#include "bgi/common.h"

/* CRC-32 (the polynomial of zlib, PNG and Ethernet) of `n` bytes, continuing
 * from `crc`: 0 for the first call, the previous result after that. */
uint32_t BGI_Crc32(uint32_t crc, const void* data, size_t n);

/* Compress `n` bytes into a zlib stream (free() the result), `*outSize`
 * bytes.  `level` 0 stores, 1 .. 9 trade time for size (the length of the
 * hash chains searched; 6 is a good middle).  NULL when memory runs out. */
uint8_t* Deflate_Compress(const uint8_t* data, size_t n, int level, size_t* outSize);

/* Decompress the zlib stream of `n` bytes (free() the result), `*outSize`
 * bytes; `hint` is the expected size (0: unknown), used for the first
 * allocation only.  NULL when the stream is malformed, its check value is
 * wrong or memory runs out. */
uint8_t* Deflate_Uncompress(const uint8_t* data, size_t n, size_t hint, size_t* outSize);

/* The code lengths of a Huffman code for `n` symbols (at most
 * HUFFMAN_SYMBOLS_MAX) with the frequencies `freq`: an optimal prefix
 * code whose lengths are limited to `maxBits`, 0 for a symbol that does
 * not occur; the most frequent symbols get the shortest codes, and a
 * single symbol gets the length 1.  Shared with the DSC encoder. */
#define HUFFMAN_SYMBOLS_MAX 512
void BGI_HuffmanLengths(const uint32_t* freq, int n, int maxBits, uint8_t* lens);

#endif // BGI_DEFLATE_H_
