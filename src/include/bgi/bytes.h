/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bytes.h - numbers in the bytes of a file: 16, 32 and 64 bits, the low
 *           byte first (Le) or the high byte first (Be), read from and
 *           written to a byte pointer whatever the machine's own order
 */
#ifndef BGI_BYTES_H_
#define BGI_BYTES_H_

#include <stdint.h>

static inline uint32_t Le16(const uint8_t* p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static inline uint32_t Le32(const uint8_t* p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t Le64(const uint8_t* p)
{
	return (uint64_t)Le32(p) | (uint64_t)Le32(p + 4) << 32;
}

static inline uint32_t Be16(const uint8_t* p)
{
	return (uint32_t)p[0] << 8 | (uint32_t)p[1];
}

static inline uint32_t Be32(const uint8_t* p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static inline void PutLe16(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void PutLe32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline void PutBe32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

#endif // BGI_BYTES_H_
