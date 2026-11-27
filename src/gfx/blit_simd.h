/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * blit_simd.h - sixteen bytes at a time: the few vector operations the
 *               row loops need (blit_rows.c), in SSE2 and in NEON
 *
 * The blitters' sums are the original's MMX ones: a byte of colour
 * widened to 16 bits, multiplied, shifted, packed back with saturation.
 * SSE2 and NEON have the same lanes and the same operations, twice as
 * wide, so a loop written with the names below is the original's
 * arithmetic four pixels at a time, on both.
 *
 * Which one is decided when the engine is built, by what the target
 * always has: SSE2 on every 64-bit x86 (and a 32-bit build made with
 * -msse2), NEON on every 64-bit ARM (and a 32-bit build made with it).
 * BLIT_SIMD is 0 anywhere else, and with -DBGI_NO_SIMD: the row loops
 * are then the word-lane ones alone.  Nothing is asked of the processor
 * while the engine runs.
 *
 * A Vec is sixteen bytes: four pixels, or - a half of them widened -
 * eight lanes of 16 bits, blue, green, red, alpha, twice.
 */
#ifndef BGI_GFX_BLIT_SIMD_H_
#define BGI_GFX_BLIT_SIMD_H_

#include <stdint.h>

#if !defined(BGI_NO_SIMD) && defined(__SSE2__)

#include <emmintrin.h>

#define BLIT_SIMD 1
#define BLIT_SIMD_NAME "SSE2"

typedef __m128i Vec;

#define V_Load(p)       _mm_loadu_si128((const __m128i*)(const void*)(p)) // (no alignment asked)
#define V_Store(p, v)   _mm_storeu_si128((__m128i*)(void*)(p), (v))
#define V_Zero()        _mm_setzero_si128()
#define V_Rep32(x)      _mm_set1_epi32((int)(x))                  // the same 32 bits four times
#define V_Rep16(x)      _mm_set1_epi16((short)(x))                // the same 16 bits eight times
#define V_Lo16(v)       _mm_unpacklo_epi8((v), _mm_setzero_si128()) // the first two pixels' bytes as 16-bit lanes
#define V_Hi16(v)       _mm_unpackhi_epi8((v), _mm_setzero_si128()) // the last two's
#define V_Pack(lo, hi)  _mm_packus_epi16((lo), (hi))              // back to bytes, 0 .. 255 (packuswb)
#define V_Add16(a, b)   _mm_add_epi16((a), (b))
#define V_Sub16(a, b)   _mm_sub_epi16((a), (b))
#define V_Mul16(a, b)   _mm_mullo_epi16((a), (b))                 // the low 16 bits of each product
#define V_Shr16(v, n)   _mm_srli_epi16((v), (n))
#define V_Shr32(v, n)   _mm_srli_epi32((v), (n))
#define V_Shl32(v, n)   _mm_slli_epi32((v), (n))
#define V_And(a, b)     _mm_and_si128((a), (b))
#define V_Or(a, b)      _mm_or_si128((a), (b))
#define V_AndNot(m, v)  _mm_andnot_si128((m), (v))                // v where m is clear
#define V_Sel(m, a, b)  _mm_or_si128(_mm_and_si128((m), (a)), _mm_andnot_si128((m), (b))) // a where m is set, b elsewhere
#define V_Eq16(a, b)    _mm_cmpeq_epi16((a), (b))                 // all ones in the lanes that are equal
#define V_Eq32(a, b)    _mm_cmpeq_epi32((a), (b))
#define V_AddSat8(a, b) _mm_adds_epu8((a), (b))                   // paddusb
#define V_IsZero(v)     (_mm_movemask_epi8(_mm_cmpeq_epi8((v), _mm_setzero_si128())) == 0xffff)

#elif !defined(BGI_NO_SIMD) && (defined(__ARM_NEON) || defined(__ARM_NEON__)) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__

#include <arm_neon.h>

#define BLIT_SIMD 1
#define BLIT_SIMD_NAME "NEON"

typedef uint8x16_t Vec;

#define V_U16(v)        vreinterpretq_u16_u8(v)
#define V_U32(v)        vreinterpretq_u32_u8(v)
#define V_From16(v)     vreinterpretq_u8_u16(v)
#define V_From32(v)     vreinterpretq_u8_u32(v)

#define V_Load(p)       vld1q_u8((const uint8_t*)(const void*)(p))
#define V_Store(p, v)   vst1q_u8((uint8_t*)(void*)(p), (v))
#define V_Zero()        vdupq_n_u8(0)
#define V_Rep32(x)      V_From32(vdupq_n_u32((uint32_t)(x)))
#define V_Rep16(x)      V_From16(vdupq_n_u16((uint16_t)(x)))
#define V_Lo16(v)       V_From16(vmovl_u8(vget_low_u8(v)))
#define V_Hi16(v)       V_From16(vmovl_u8(vget_high_u8(v)))
#define V_Pack(lo, hi)  vcombine_u8(vqmovun_s16(vreinterpretq_s16_u8(lo)), vqmovun_s16(vreinterpretq_s16_u8(hi)))
#define V_Add16(a, b)   V_From16(vaddq_u16(V_U16(a), V_U16(b)))
#define V_Sub16(a, b)   V_From16(vsubq_u16(V_U16(a), V_U16(b)))
#define V_Mul16(a, b)   V_From16(vmulq_u16(V_U16(a), V_U16(b)))
#define V_Shr16(v, n)   V_From16(vshrq_n_u16(V_U16(v), (n)))
#define V_Shr32(v, n)   V_From32(vshrq_n_u32(V_U32(v), (n)))
#define V_Shl32(v, n)   V_From32(vshlq_n_u32(V_U32(v), (n)))
#define V_And(a, b)     vandq_u8((a), (b))
#define V_Or(a, b)      vorrq_u8((a), (b))
#define V_AndNot(m, v)  vbicq_u8((v), (m))
#define V_Sel(m, a, b)  vbslq_u8((m), (a), (b))
#define V_Eq16(a, b)    V_From16(vceqq_u16(V_U16(a), V_U16(b)))
#define V_Eq32(a, b)    V_From32(vceqq_u32(V_U32(a), V_U32(b)))
#define V_AddSat8(a, b) vqaddq_u8((a), (b))
#define V_IsZero(v)     ((vgetq_lane_u64(vreinterpretq_u64_u8(v), 0) | vgetq_lane_u64(vreinterpretq_u64_u8(v), 1)) == 0)

#else

#define BLIT_SIMD 0
#define BLIT_SIMD_NAME "none"

#endif

#endif // BGI_GFX_BLIT_SIMD_H_
