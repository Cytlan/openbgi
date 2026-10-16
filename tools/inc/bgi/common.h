/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * common.h - basic types, macros and conventions shared by every module.
 *
 * OpenBGI is a C99 reimplementation of the "Buriko General Interpreter"
 * (BGI / Ethornell) visual-novel engine, written against tayutama.exe
 * (version string in the binary: "ver 1.69 ( build : 444.2 )").  Other
 * builds of the engine are selected through the profiles of version.h.
 *
 * Conventions used throughout the sources (see CONVENTIONS.md)
 * -------------------------------------------------------------
 *  * The original is MSVC 6 C++.  Classes become structs with the vtable
 *    pointer first, methods become functions taking the object as their
 *    first argument, and virtual tables become structs of function
 *    pointers.  The C layout does not try to match the original's.
 *  * Script-visible integers are 32-bit (int32_t / uint32_t).  Script
 *    pointers are 32-bit tagged values, see vm.h and version.h.
 *  * Rectangles are *inclusive* (right/bottom are the last pixel), exactly
 *    as in the original.
 *  * Text is Shift-JIS (CP932) everywhere, including file names and error
 *    messages; "81 00" of 1.653 on switches the script text to UTF-8 (see
 *    strutil.h).  The Japanese messages of the original live in
 *    tools/messages.txt and reach the code as the MSG_* macros of
 *    inc/bgi/msg.h: English by default, the originals with --lang=ja.
 */
#ifndef BGI_COMMON_H_
#define BGI_COMMON_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// -------------------------------------------------------------------------
// Platform selection
// -------------------------------------------------------------------------
#if defined(_WIN32)
#define BGI_WIN32 1
#else
#define BGI_POSIX 1
#endif

// -------------------------------------------------------------------------
// Small helpers
// -------------------------------------------------------------------------
#define BGI_UNUSED(x)        ((void)(x))
#define BGI_COUNTOF(a)       (sizeof(a) / sizeof((a)[0]))
#define BGI_MIN(a, b)        ((a) < (b) ? (a) : (b))
#define BGI_MAX(a, b)        ((a) > (b) ? (a) : (b))
#define BGI_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define BGI_ABS(v)           ((v) < 0 ? -(v) : (v))

/* Marks the functions that end in a throw (C99 has no _Noreturn); it lets
 * the compilers see that the code after a script error is unreachable. */
#if defined(__GNUC__)
#define BGI_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#define BGI_NORETURN __declspec(noreturn)
#else
#define BGI_NORETURN
#endif

/* The MSVC 6 CRT generator the engine links (rand / srand): a 32-bit LCG
 * whose high 15 bits are returned.  Scripts depend on the exact sequence
 * (seeded by "80 00", drawn by "80 01" / "80 02"), so the C library's
 * rand() is never used.  Implemented in src/core/sysutil.c. */
void BGI_Srand(uint32_t seed);
int BGI_Rand(void); // 0 .. 0x7fff

/* The original uses the MSVC CRT's operator new, which never returns NULL
 * in practice (it calls the new-handler).  BGI_Alloc() aborts on failure so
 * that the rest of the code can skip the NULL checks the original skips.
 * Implemented in src/core/sysutil.c. */
void* BGI_Alloc(size_t n);       // uninitialised, like operator new; n = 0 gives a 1-byte block
void* BGI_Calloc(size_t n);      // zero filled
void BGI_Free(void* p);          // NULL is allowed
char* BGI_Strdup(const char* s); // copy with BGI_Alloc

/* Result codes used by many engine objects: 0 = success, 0x80000000.. =
 * specific failures.  The "Gfx_*" wrappers translate them to small ints. */
#define BGI_OK       0u
#define BGI_ERR_BASE 0x80000000u
#define BGI_ERR(n)   (BGI_ERR_BASE | (uint32_t)(n))

// Fixed point conventions
#define FX16_ONE     0x10000 // 16.16
#define FX16_HALF    0x8000
#define PROGRESS_ONE 0x1000000 // 8.16 animation progress: script 0x100 << 16
#define LEVEL_MAX    0x100     // "level" (transparency / strength) 0..0x100

// a * b / c with a signed 64-bit intermediate, truncating toward zero (the CRT's _allmul / _alldiv)
static inline int32_t BGI_Muldiv(int32_t a, int32_t b, int32_t c)
{
	return (int32_t)(((int64_t)a * (int64_t)b) / (int64_t)c);
}

// Unsigned / signed wrap-around helpers for 32-bit script arithmetic
static inline uint32_t U32Add(uint32_t a, uint32_t b)
{
	return (uint32_t)(a + b);
}

// shift left by n mod 32, as the x86 shl instruction does (no undefined behaviour on overflow)
static inline int32_t S32Shl(int32_t a, unsigned n)
{
	return (int32_t)((uint32_t)a << (n & 31));
}

/* MSVC _ftol: truncation toward zero; out-of-range and NaN give the x87
 * "integer indefinite" 0x80000000, which the blitters inherit. */
static inline int32_t BGI_Ftol(double d)
{
	if(!(d > -2147483649.0 && d < 2147483648.0))
		return (int32_t)0x80000000;
	return (int32_t)d;
}

#endif // BGI_COMMON_H_
