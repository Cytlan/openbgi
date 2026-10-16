/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ops_base.c - the base instructions of the script VM (the main table,
 * opcodes 00 .. 7F), the VM's sprintf, the watch-mode variants of the
 * writing instructions, the dispatchers of the two-byte families and the
 * filling of the handler tables.  Interface: bgi/vm.h.
 *
 * Every handler has the shape  int op(Thread_t* t)  and returns a scheduler
 * code (0 = continue; docs/vm.md lists the others).  The trailing comment on
 * a handler's signature line gives the opcode and the stack effect
 * "inputs → outputs": the inputs in the order the script pushed them (the
 * last one is on top of the stack and is popped first); the outputs as they
 * are pushed, the last one ending on top.
 * Operands that follow the opcode in the code stream (immediates, size
 * bytes, condition codes) are described in the comment above the handler.
 *
 * The handler tables are static data in the original, one set per engine
 * version.  Here Vm_TablesInit fills them at start-up from registration
 * lists (OpEntry_t) for the selected engine profile, so the one binary
 * runs the scripts of several versions.
 */
#include "bgi/vm.h"
#include "bgi/sys.h"
#include "bgi/error.h"
#include "bgi/strutil.h"
#include "bgi/msg.h"
#include "bgi/gfx.h"
#include "bgi/gfx/bmpops.h"
#include "bgi/os.h"
#include <math.h>

// ---- push immediates --------------------------------------------------------

// the signed byte after the opcode, sign-extended
static int Opcode_PushI8(Thread_t* t) // 00: → v
{
	Thread_Push(t, (uint32_t)(int32_t)(int8_t)RdU8(t));
	return 0;
}

// the signed 16-bit word after the opcode, sign-extended
static int Opcode_PushI16(Thread_t* t) // 01: → v
{
	Thread_Push(t, (uint32_t)(int32_t)(int16_t)RdU16(t));
	return 0;
}

static int Opcode_PushI32(Thread_t* t) // 02: → v
{
	Thread_Push(t, RdU32(t));
	return 0;
}

/* the address of a local: the frame pointer minus the 16-bit offset after
 * the opcode, tagged as the thread's data area (VM_TAG_DATA) */
static int Opcode_PushLocalAddr(Thread_t* t) // 04: → ptr
{
	uint32_t n = RdU16(t);
	Thread_Push(t, (Thread_GetFP(t) - n) | VM_TAG_DATA);
	return 0;
}

/* the address of data embedded in the program: this instruction's position
 * plus the signed 16-bit displacement after the opcode, tagged as the
 * thread's code area (VM_TAG_CODE) */
static int Opcode_PushCodeAddr(Thread_t* t) // 05: → ptr
{
	int32_t d = (int16_t)RdU16(t);
	Thread_Push(t, (uint32_t)((int32_t)Thread_CurOpIP(t) + d) | VM_TAG_CODE);
	return 0;
}

/* a jump or call target: this instruction's position plus the signed 16-bit
 * displacement after the opcode, untagged (as "14" .. "16" expect it) */
static int Opcode_PushCodeOff(Thread_t* t) // 06: → addr
{
	int32_t d = (int16_t)RdU16(t);
	Thread_Push(t, (uint32_t)((int32_t)Thread_CurOpIP(t) + d));
	return 0;
}

// ---- loads and stores ------------------------------------------------------

/* read one element through a script pointer.  The size byte after the
 * opcode selects it: 0 a signed byte, 1 a signed 16-bit word, 2 a 32-bit
 * dword.  Any other size byte reads an uninitialised stack slot in the
 * original; here 0 is pushed. */
static int Opcode_Load(Thread_t* t) // 08: ptr → v
{
	uint8_t* p = (uint8_t*)PopPtr(t);
	uint8_t sz = RdU8(t);
	uint32_t v;
	switch(sz)
	{
		case 0: v = (uint32_t)(int32_t)(int8_t)p[0]; break;
		case 1:
		{
			int16_t s;
			memcpy(&s, p, 2);
			v = (uint32_t)(int32_t)s;
			break;
		}
		case 2: memcpy(&v, p, 4); break;
		default: v = 0; break;
	}
	Thread_Push(t, v);
	return 0;
}

/* write `val` through p as the element the size code `sz` selects (0 byte,
 * 1 16-bit word, 2 dword, as in Opcode_Load); any other code writes nothing */
static void StoreSized(uint8_t* p, uint32_t sz, uint32_t val)
{
	switch(sz)
	{
		case 0: p[0] = (uint8_t)val; break;
		case 1:
		{
			uint16_t s = (uint16_t)val;
			memcpy(p, &s, 2);
			break;
		}
		case 2: memcpy(p, &val, 4); break;
		default: break;
	}
}

// store with the size byte after the opcode, leaving the value on the stack
static int Opcode_StoreKeep(Thread_t* t) // 09: ptr, v → v
{
	uint32_t v = Thread_Pop(t);
	uint8_t* p = (uint8_t*)PopPtr(t);
	uint32_t sz = RdU8(t);
	StoreSized(p, sz, v);
	Thread_Push(t, v);
	return 0;
}

// store with the size byte after the opcode
static int Opcode_Store(Thread_t* t) // 0A: v, ptr →
{
	uint8_t* p = (uint8_t*)PopPtr(t);
	uint32_t v = Thread_Pop(t);
	uint32_t sz = RdU8(t);
	StoreSized(p, sz, v);
	return 0;
}

/* copy a literal from the code stream to *ptr: a count byte follows the
 * opcode, then that many bytes (a string constant, typically) */
static int Opcode_StoreInline(Thread_t* t) // 0B: ptr →
{
	uint8_t* p = (uint8_t*)PopPtr(t);
	uint32_t n = RdU8(t);
	RdBytes(t, p, n);
	return 0;
}

/* store cnt values of one size in consecutive elements from *ptr (an array
 * initialiser); the two bytes after the opcode are the size code and the
 * count.  The value pushed first lands at the lowest address. */
static int Opcode_StoreMulti(Thread_t* t) // 0C: ptr, v[cnt] →
{
	static const uint32_t elemSize[3] = {1, 2, 4};
	uint32_t vals[0x100];
	uint32_t sz = RdU8(t), cnt = RdU8(t), i;
	uint8_t* p;
	for(i = 0; i < cnt; i++)
		vals[i] = Thread_Pop(t);
	p = (uint8_t*)PopPtr(t);
	for(i = cnt; i > 0; i--)
	{
		StoreSized(p, sz, vals[i - 1]);
		p += elemSize[sz & 3]; // a size code above 2 reads past the table, as in the original
	}
	return 0;
}

// ---- frame pointer and control flow ---------------------------------------

static int Opcode_PushFp(Thread_t* t) // 10: → fp
{
	Thread_Push(t, Thread_GetFP(t));
	return 0;
}

// a frame pointer at or beyond the end of the data area is a script error
static int Opcode_SetFp(Thread_t* t) // 11: v →
{
	uint32_t v = Thread_Pop(t);
	if(v >= t->dataSize)
	{
		char msg[0x100];
		sprintf(msg, MSG_SP_OUT_OF_RANGE, (unsigned)v);
		ScriptError(msg, t);
	}
	Thread_SetFP(t, v);
	return 0;
}

// a target of 0, or one beyond the end of the code area, is a script error
static int Opcode_Jmp(Thread_t* t) // 14: addr →
{
	uint32_t a = Thread_Pop(t);
	if(a == 0)
		ScriptError(MSG_JUMP_TO_NULL, t);
	if(a >= t->codeSize)
	{
		char msg[0x100];
		sprintf(msg, MSG_IP_OUT_OF_RANGE, (unsigned)a);
		ScriptError(msg, t);
	}
	Thread_SetIP(t, a);
	return 0;
}

/* conditional jump.  The condition byte after the opcode compares v with 0
 * as a signed value (0: != 0, 1: == 0, 2: > 0, 3: >= 0, 4: <= 0, 5: < 0);
 * the jump is taken when the condition holds, and a target beyond the code
 * area is then a script error.  A condition above 5 tests an uninitialised
 * local in the original; here it is never taken. */
static int Opcode_Jcc(Thread_t* t) // 15: v, addr →
{
	uint32_t a = Thread_Pop(t);
	int32_t v = (int32_t)Thread_Pop(t);
	uint8_t cc = RdU8(t);
	int take;
	switch(cc)
	{
		case 0: take = v != 0; break;
		case 1: take = v == 0; break;
		case 2: take = v > 0; break;
		case 3: take = v >= 0; break;
		case 4: take = v <= 0; break;
		case 5: take = v < 0; break;
		default: take = 0; break;
	}
	if(take)
	{
		if(a >= t->codeSize)
		{
			char msg[0x100];
			sprintf(msg, MSG_IP_OUT_OF_RANGE, (unsigned)a);
			ScriptError(msg, t);
		}
		Thread_SetIP(t, a);
	}
	return 0;
}

/* push the return address (the instruction after this one) on the frame
 * stack in the data area, then jump as "14" does; a frame stack that would
 * run past the data area is a script error */
static int Opcode_Call(Thread_t* t) // 16: addr →
{
	if(Thread_GetFP(t) + 4 >= t->dataSize)
		ScriptError(MSG_STACK_FULL, t);
	FramePush(t, Thread_CurOpIP(t) + 1);
	return Opcode_Jmp(t);
}

// return to the address on the frame stack; at the outermost frame the thread ends
static int Opcode_Ret(Thread_t* t) // 17: →
{
	if(Thread_GetFP(t) == 0)
		return 4; // outermost frame: the thread ends
	Thread_SetIP(t, FramePop(t));
	return 0;
}

// ---- arithmetic (b is on top) ----------------------------------------------

/* defines the handler of a binary operator: it pops b (the top of the
 * stack) and a, and pushes `expr`, an expression in the uint32_t values a
 * and b (the signed operators cast inside it).  The comparisons and the
 * logical operators push 0 or 1. */
#define BINOP(name, expr)                              \
	static int name(Thread_t* t) /* a, b → a op b */ \
	{                                                  \
		uint32_t b = Thread_Pop(t), a = Thread_Pop(t); \
		Thread_Push(t, (uint32_t)(expr));              \
		return 0;                                      \
	}

BINOP(Opcode_Add, a + b)                   // 20: a, b → a + b
BINOP(Opcode_Sub, a - b)                   // 21: a, b → a - b
BINOP(Opcode_Mul, (int32_t)a*(int32_t)b)   // 22: a, b → a * b (the low 32 bits)
BINOP(Opcode_And, a& b)                    // 25: a, b → a & b
BINOP(Opcode_Or, a | b)                    // 26: a, b → a | b
BINOP(Opcode_Xor, a ^ b)                   // 27: a, b → a ^ b
BINOP(Opcode_Shl, a << (b & 31))           // 29: a, b → a << b (the count masked to 5 bits, as the x86 does)
BINOP(Opcode_Shr, a >> (b & 31))           // 2A: a, b → a >> b (logical)
BINOP(Opcode_Sar, (int32_t)a >> (b & 31))  // 2B: a, b → a >> b (arithmetic)
BINOP(Opcode_Eq, a == b)                   // 30: a, b → a == b
BINOP(Opcode_Ne, a != b)                   // 31: a, b → a != b
BINOP(Opcode_Le, (int32_t)a <= (int32_t)b) // 32: a, b → a <= b (signed)
BINOP(Opcode_Ge, (int32_t)a >= (int32_t)b) // 33: a, b → a >= b (signed)
BINOP(Opcode_Lt, (int32_t)a < (int32_t)b)  // 34: a, b → a < b (signed)
BINOP(Opcode_Gt, (int32_t)a > (int32_t)b)  // 35: a, b → a > b (signed)
BINOP(Opcode_Land, (a != 0 && b != 0))     // 38: a, b → a && b
BINOP(Opcode_Lor, (a != 0 || b != 0))      // 39: a, b → a || b

/* signed division; a zero divisor gives -1 instead of a fault.  INT_MIN /
 * -1 overflows: the original's idiv faults, here the C division's behaviour
 * is undefined (a trap on x86 as well). */
static int Opcode_Div(Thread_t* t) // 23: a, b → a / b
{
	int32_t b = (int32_t)Thread_Pop(t), a = (int32_t)Thread_Pop(t);
	Thread_Push(t, b == 0 ? 0xffffffffu : (uint32_t)(a / b));
	return 0;
}

// signed remainder (the sign of the dividend); a zero divisor gives -1
static int Opcode_Mod(Thread_t* t) // 24: a, b → a % b
{
	int32_t b = (int32_t)Thread_Pop(t), a = (int32_t)Thread_Pop(t);
	Thread_Push(t, b == 0 ? 0xffffffffu : (uint32_t)(a % b));
	return 0;
}

static int Opcode_Not(Thread_t* t) // 28: a → ~a
{
	Thread_Push(t, ~Thread_Pop(t));
	return 0;
}

static int Opcode_Lnot(Thread_t* t) // 3A: a → !a
{
	Thread_Push(t, Thread_Pop(t) == 0);
	return 0;
}

static int Opcode_Select(Thread_t* t) // 40: c, tv, f → c ? tv : f
{
	uint32_t f = Thread_Pop(t), tv = Thread_Pop(t), c = Thread_Pop(t);
	Thread_Push(t, c ? tv : f);
	return 0;
}

/* a * b / c with a 64-bit intermediate product (signed); a zero divisor
 * gives 0 */
static int Opcode_Muldiv(Thread_t* t) // 42: a, b, c → a * b / c
{
	int32_t c = (int32_t)Thread_Pop(t), b = (int32_t)Thread_Pop(t), a = (int32_t)Thread_Pop(t);
	int64_t p = (int64_t)a * (int64_t)b;
	// the original's 64-bit division by zero raises the CRT's integer-divide exception
	Thread_Push(t, c == 0 ? 0 : (uint32_t)(int32_t)(p / c));
	return 0;
}

// radians per unit of an angle in 16.16 degrees: 2 * pi / (65536 * 360)
#define ANGLE_SCALE 2.663161090079238e-07

// the sine of an angle in 16.16 degrees; the result is 16.16, truncated
static int Opcode_Sin(Thread_t* t) // 48: angle → sin
{
	int32_t a = (int32_t)Thread_Pop(t);
	Thread_Push(t, (uint32_t)(int32_t)(sin((double)a * ANGLE_SCALE) * 65536.0));
	return 0;
}

// the cosine of an angle in 16.16 degrees; the result is 16.16, truncated
static int Opcode_Cos(Thread_t* t) // 49: angle → cos
{
	int32_t a = (int32_t)Thread_Pop(t);
	Thread_Push(t, (uint32_t)(int32_t)(cos((double)a * ANGLE_SCALE) * 65536.0));
	return 0;
}

/* 1.69 build 472 on: atan2(y, x) as an angle in 16.16 degrees, 0 .. 360,
 * measured from the positive x axis towards the positive y axis.  On the
 * y axis (x == 0) the result is 0, 90 or 270 without a computation; the
 * rest is a = atan(y / x) scaled to 16.16 degrees and truncated (-90 .. 90),
 * folded into the right quadrant: x > 0 gives a, plus 360 when y < 0;
 * x < 0 gives 180 + a. */
static int Opcode_Atan2(Thread_t* t) // 43 (1.69/472 on): y, x → angle
{
	int32_t x = (int32_t)Thread_Pop(t);
	int32_t y = (int32_t)Thread_Pop(t);
	const double k = 180.0 / 3.141592653589793 * 65536.0;
	int32_t r;
	if(x == 0)
		r = y == 0 ? 0 : y > 0 ? 0x5a0000
							   : 0x10e0000;
	else
	{
		double a = atan((double)y / (double)x); // the sign of y for x > 0, reversed for x < 0
		if(x > 0)
			r = y < 0 ? 0x1680000 - (int32_t)(a * -k) : (int32_t)(a * k);
		else
			r = 0xb40000 - (int32_t)(a * -k);
	}
	Thread_Push(t, (uint32_t)r);
	return 0;
}

/* the 64-bit arithmetic of "50" .. "54" (1.69 build 472 on): *dst = *a op *b
 * on three script pointers to 64-bit values stored as two little-endian
 * dwords each.  `op` is 0 add, 1 subtract, 2 multiply, 3 divide, 4
 * remainder; division and remainder are signed.  A zero divisor raises the
 * CRT's divide exception in the original; here the result is 0, as is the
 * remainder by -1, and the quotient by -1 is the plain negation (so INT64_MIN
 * cannot overflow the division). */
static void Int64Op(Thread_t* t, int op)
{
	uint32_t* b = (uint32_t*)PopPtr(t);
	uint32_t* a = (uint32_t*)PopPtr(t);
	uint32_t* d = (uint32_t*)PopPtr(t);
	int64_t va = (int64_t)(((uint64_t)a[1] << 32) | a[0]);
	int64_t vb = (int64_t)(((uint64_t)b[1] << 32) | b[0]);
	int64_t r;
	switch(op)
	{
		case 0: r = (int64_t)((uint64_t)va + (uint64_t)vb); break;
		case 1: r = (int64_t)((uint64_t)va - (uint64_t)vb); break;
		case 2: r = (int64_t)((uint64_t)va * (uint64_t)vb); break;
		case 3: r = vb == 0 ? 0 : (vb == -1 ? (int64_t)(0 - (uint64_t)va) : va / vb); break;
		default: r = vb == 0 || vb == -1 ? 0 : va % vb; break;
	}
	d[0] = (uint32_t)r;
	d[1] = (uint32_t)((uint64_t)r >> 32);
}
static int Opcode_Add64(Thread_t* t) // 50 (1.69/472 on): dst, a, b →
{
	Int64Op(t, 0);
	return 0;
}
static int Opcode_Sub64(Thread_t* t) // 51 (1.69/472 on): dst, a, b →
{
	Int64Op(t, 1);
	return 0;
}
static int Opcode_Mul64(Thread_t* t) // 52 (1.69/472 on): dst, a, b →
{
	Int64Op(t, 2);
	return 0;
}
static int Opcode_Div64(Thread_t* t) // 53 (1.69/472 on): dst, a, b →
{
	Int64Op(t, 3);
	return 0;
}
static int Opcode_Mod64(Thread_t* t) // 54 (1.69/472 on): dst, a, b →
{
	Int64Op(t, 4);
	return 0;
}

// 1.69 build 472 on: put the string on the clipboard as CF_TEXT; 1 when done
static int Opcode_ClipboardSet(Thread_t* t) // 7E (1.69/472 on): text → ok
{
	const char* text = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)OS_ClipboardSetText(text));
	return 0;
}

// ---- memory and strings ----------------------------------------------------

static int Opcode_Memcpy(Thread_t* t) // 60: dst, src, n →
{
	uint32_t n = Thread_Pop(t);
	void* src = PopPtr(t);
	void* dst = PopPtr(t);
	memcpy(dst, src, n); // the original is a forward rep movs; memcpy assumes the ranges do not overlap
	return 0;
}

static int Opcode_Memclr(Thread_t* t) // 61: dst, n →
{
	uint32_t n = Thread_Pop(t);
	void* dst = PopPtr(t);
	memset(dst, 0, n);
	return 0;
}

// only the low byte of the value is used
static int Opcode_Memset(Thread_t* t) // 62: dst, n, val →
{
	uint32_t val = Thread_Pop(t), n = Thread_Pop(t);
	void* dst = PopPtr(t);
	memset(dst, (int)(val & 0xff), n);
	return 0;
}

static int Opcode_Memcmp(Thread_t* t) // 63: a, b, n → eq
{
	uint32_t n = Thread_Pop(t);
	void* b = PopPtr(t);
	void* a = PopPtr(t);
	Thread_Push(t, memcmp(a, b, n) == 0);
	return 0;
}

/* 1.529 on: the byte offset of the first occurrence of `pat` in `s`, or -1
 * when there is none.  A plain byte search (the CRT strstr), unlike the
 * Shift-JIS-aware search of "67". */
static int Opcode_Strstr(Thread_t* t) // 66 (1.529 on): s, pat → off
{
	const char* pat = (const char*)PopPtr(t);
	const char* s = (const char*)PopPtr(t);
	const char* f = strstr(s, pat);
	Thread_Push(t, f ? (uint32_t)(f - s) : 0xffffffffu);
	return 0;
}

/* copy src to dst with every occurrence of `old` replaced by `new` (the
 * search steps over Shift-JIS characters); pushes the number of replacements */
static int Opcode_Strreplace(Thread_t* t) // 67: dst, src, old, new → count
{
	char* nw = (char*)PopPtr(t);
	char* old = (char*)PopPtr(t);
	char* src = (char*)PopPtr(t);
	char* dst = (char*)PopPtr(t);
	Thread_Push(t, (uint32_t)StrReplace(dst, src, old, nw));
	return 0;
}

static int Opcode_Strlen(Thread_t* t) // 68: s → len
{
	Thread_Push(t, (uint32_t)strlen((char*)PopPtr(t)));
	return 0;
}

static int Opcode_Streq(Thread_t* t) // 69: a, b → eq
{
	char* b = (char*)PopPtr(t);
	char* a = (char*)PopPtr(t);
	Thread_Push(t, strcmp(a, b) == 0);
	return 0;
}

/* The string instructions below take their source and destination from the
 * scripts, which freely pass the same buffer as both: the scenario
 * libraries build every file name with `strcat(buf, buf, "x")`, and a
 * `strcpy(s, s)` follows a lower-casing.  The original hands such calls to
 * the CRT's strcpy / sprintf, which write the output as they read the
 * input and so simply append (or copy nothing).  Another CRT may not: the
 * glibc sprintf clears the destination before it reads the arguments, and
 * `strcat(buf, buf, "_N")` then leaves "_N" (the bust shot "bs_a11_d_N"
 * came out as "_N" in Heptagram's fast forward).  So the results are
 * assembled beside the destination and copied in whole, which is what the
 * original's behaviour amounts to for every overlap the scripts produce. */

// copy `n` bytes (NUL included) assembled from up to three pieces into dst, which may overlap any of them
static void StrAssemble(char* dst, const char* a, size_t la, const char* b, size_t lb, const char* c, size_t lc)
{
	char small[0x200];
	size_t n = la + lb + lc + 1;
	char* tmp = n <= sizeof small ? small : (char*)malloc(n);
	memcpy(tmp, a, la);
	memcpy(tmp + la, b, lb);
	memcpy(tmp + la + lb, c, lc);
	tmp[la + lb + lc] = 0;
	memcpy(dst, tmp, n);
	if(tmp != small)
		free(tmp);
}

static int Opcode_Strcpy(Thread_t* t) // 6A: dst, src →
{
	char* src = (char*)PopPtr(t);
	char* dst = (char*)PopPtr(t);
	if(dst != src)
		memmove(dst, src, strlen(src) + 1);
	return 0;
}

// dst receives a followed by b (`strcat(buf, buf, "x")` appends: see StrAssemble)
static int Opcode_Strcat2(Thread_t* t) // 6B: dst, a, b →
{
	char* b = (char*)PopPtr(t);
	char* a = (char*)PopPtr(t);
	char* dst = (char*)PopPtr(t);
	StrAssemble(dst, a, strlen(a), b, strlen(b), "", 0);
	return 0;
}

/* decode the first character of a Shift-JIS string: its code, whether it is
 * a double-byte character, and whether it is one that may not start a line
 * (kinsoku; that flag ends on top) */
static int Opcode_Getchar(Thread_t* t) // 6C: s → ch, dbcs, kinsoku
{
	char* s = (char*)PopPtr(t);
	uint32_t ch;
	int dbcs = SjisGetChar(&ch, s);
	Thread_Push(t, ch);
	Thread_Push(t, (uint32_t)dbcs);
	Thread_Push(t, (uint32_t)IsKinsokuChar(ch));
	return 0;
}

/* "6C" of 1.653 on: the string's encoding is judged from its bytes
 * (TextSniff) and the first character decoded accordingly; `wide` is the
 * full-width flag rather than "two bytes".  From 1.654 on the byte length of
 * the character is pushed first: s → len, ch, wide, kinsoku. */
static int Opcode_Getchar_653(Thread_t* t) // 6C (1.653 on): s → ch, wide, kinsoku
{
	char* s = (char*)PopPtr(t);
	uint32_t ch;
	int wide;
	int len = TextGetCharMode(&ch, &wide, s, TextSniff(s));
	if(gEngine->gen >= GEN_1_654)
		Thread_Push(t, (uint32_t)len);
	Thread_Push(t, ch);
	Thread_Push(t, (uint32_t)wide);
	Thread_Push(t, (uint32_t)IsKinsokuChar(ch));
	return 0;
}

// lower-case the single-byte letters of a Shift-JIS string in place
static int Opcode_Strlwr(Thread_t* t) // 6D: s →
{
	SjisStrLwr((char*)PopPtr(t));
	return 0;
}

// dst receives src enclosed in the character ch on both sides
static int Opcode_Strquote(Thread_t* t) // 6E: dst, src, ch →
{
	uint32_t ch = Thread_Pop(t);
	char* src = (char*)PopPtr(t);
	char* dst = (char*)PopPtr(t);
	char q[8];
	size_t lq = 1;
	if(gTextUtf8)
		lq = (size_t)TextPutChar(q, ch); // the quote character as the bytes of the current encoding
	else
		q[0] = (char)ch;
	q[lq] = 0;
	StrAssemble(dst, q, lq, src, strlen(src), q, lq);
	return 0;
}

/*
 * the sprintf of "6F".  The format is scanned once; for every conversion
 * one argument is popped from the evaluation stack in order of appearance:
 * an integer for %d %x %X %c, a script pointer for %s.  "%%" is a literal
 * percent sign.  One flag character (' ', '0', '-', '.') followed by digits
 * is accepted; any other conversion character is a script error, and so are
 * more than 16 conversions.  The original collects the arguments and calls
 * the CRT sprintf once; here each conversion is formatted on its own, which
 * produces the same text.  The text is formatted aside and copied into dst
 * at the end, so that an argument may be the destination itself (the note
 * at StrAssemble above).
 */
static void VmSprintf(Thread_t* t, char* dstOut, const char* fmt)
{
	const char* p = fmt;
	int count = 0;
	char spec[32];
	char out[0x2000]; // the longest text a script formats is a few hundred bytes; a longer one is cut here
	char* dst = out;
	char* const end = out + sizeof out - 1;

	*dst = 0;
	while(*p)
	{
		if(*p != '%')
		{
			if(dst < end)
				*dst++ = *p;
			p++;
			*dst = 0;
			continue;
		}
		{
			const char* start = p;
			const char* q = p + 1;
			char type;
			size_t n;
			if(*q == ' ' || *q == '0' || *q == '-' || *q == '.')
			{
				q++;
				while(*q >= '0' && *q <= '9')
					q++;
			}
			type = *q;
			if(type == 0)
			{
				ScriptError(MSG_FMT_INCOMPLETE, t);
			}
			if(type == '%')
			{
				if(dst < end)
					*dst++ = '%';
				*dst = 0;
				p = q + 1;
				continue;
			}
			n = (size_t)(q - start) + 1;
			if(n >= sizeof spec)
				n = sizeof spec - 1;
			memcpy(spec, start, n);
			spec[n] = 0;
			switch(type)
			{
				case 'd':
				case 'x':
				case 'X':
				case 'c':
				{
					int32_t v = (int32_t)Thread_Pop(t);
					int k = snprintf(dst, (size_t)(end - dst) + 1, spec, v);
					dst += k < 0 ? 0 : k > end - dst ? end - dst
													 : k;
					break;
				}
				case 's':
				{
					const char* s = (const char*)PopPtr(t);
					int k = snprintf(dst, (size_t)(end - dst) + 1, spec, s);
					dst += k < 0 ? 0 : k > end - dst ? end - dst
													 : k;
					break;
				}
				default:
				{
					char msg[0x100];
					sprintf(msg, MSG_FMT_BAD_TYPE, type);
					ScriptError(msg, t);
					break;
				}
			}
			if(count >= 16)
				ScriptError(MSG_FMT_TOO_MANY, t);
			count++;
			p = q + 1;
		}
	}
	memcpy(dstOut, out, (size_t)(dst - out) + 1);
}

/* format into dst (VmSprintf); the arguments of the conversions follow fmt
 * and dst on the stack, the first conversion's nearest the top */
static int Opcode_Sprintf(Thread_t* t) // 6F: args…, dst, fmt →
{
	char* fmt = (char*)PopPtr(t);
	char* dst = (char*)PopPtr(t);
	VmSprintf(t, dst, fmt);
	return 0;
}

// ---- thread-local heap ------------------------------------------------------

/* script error for a request above the profile's local-heap limit (32 MB in
 * 1.69); shared with the global allocation of "80 20" */
void CheckAllocSize(uint32_t size, Thread_t* t)
{
	if(size > gEngine->heapLimit)
	{
		char msg[0x100];
		sprintf(msg, MSG_ALLOC_TOO_BIG, (int)size, (int)gEngine->heapLimit);
		ScriptError(msg, t);
	}
}

/* allocate `size` bytes on the thread's local heap and push the tagged
 * pointer (VM_TAG_HEAP); a size above the heap limit, or a block that would
 * end beyond it, is a script error */
static int Opcode_Lalloc(Thread_t* t) // 70: size → p
{
	uint32_t size = Thread_Pop(t), off;
	CheckAllocSize(size, t);
	off = Thread_HeapAlloc(t, size);
	if(off + size > gEngine->heapLimit)
		ScriptError(MSG_LALLOC_FAILED, t);
	Thread_Push(t, off + VM_TAG_HEAP);
	return 0;
}

/* free a local-heap block.  A pointer outside the thread's heap, or one that
 * does not start an allocated block, is a script error; so `ok` is 1
 * whenever the instruction completes. */
static int Opcode_Lfree(Thread_t* t) // 71: p → ok
{
	uint32_t p = Thread_Pop(t);
	int ok;
	if((p & VM_TAG_MASK) != VM_TAG_HEAP)
	{
		char msg[0x100];
		sprintf(msg, MSG_NOT_LOCAL_PTR, (unsigned)p);
		ScriptError(msg, t);
	}
	ok = Thread_HeapFree(t, p & VM_OFFSET_MASK);
	if(!ok)
		ScriptError(MSG_LFREE_FAILED, t);
	Thread_Push(t, (uint32_t)ok);
	return 0;
}

// ---- watch mode (debugging aid) --------------------------------------------

/* A script registers memory regions with "75"; while watch mode ("74") is
 * on, every writing instruction checks its destination against them and
 * reports a hit in a message box.  The checked handlers replace the plain
 * ones in the main table (SetWatchMode). */

typedef struct WatchNode // one watched region
{
	uint32_t threadId;      // the thread that registered it
	uint32_t addr;          // tagged script address of its first byte
	uint32_t size;          // bytes
	char name[0x100];       // the label given to "75"
	struct WatchNode* next; // singly linked, newest first
} WatchNode_t;
static WatchNode_t* gWatchList; // the registered regions; Watch_Clear frees them

/* register [addr, addr + size) under `name` for thread t; 1 when added, 0 for
 * an empty region (nothing is registered) */
static int WatchAdd(uint32_t addr, uint32_t size, const char* name, Thread_t* t)
{
	if(size != 0)
	{
		WatchNode_t* w = (WatchNode_t*)BGI_Alloc(sizeof(WatchNode_t));
		w->threadId = Thread_GetId(t);
		w->addr = addr;
		w->size = size;
		strcpy(w->name, name);
		w->next = gWatchList;
		gWatchList = w;
	}
	return size != 0;
}

// drop every watched region (part of the machine's shutdown and reboot)
void Watch_Clear(void)
{
	WatchNode_t *w = gWatchList, *nx;
	while(w)
	{
		nx = w->next;
		BGI_Free(w);
		w = nx;
	}
	gWatchList = NULL;
}

/* report (a message box, not an error) when the write [ptr, ptr + size)
 * overlaps a watched region.  Regions inside a thread's own areas (code,
 * data, local heap) match only the thread that registered them.  `msg`
 * names the operation in the report. */
static void WatchCheckRange2(uint32_t ptr, uint32_t size, Thread_t* t, const char* msg)
{
	WatchNode_t* w;
	uint32_t last = ptr + size - 1;
	for(w = gWatchList; w; w = w->next)
		if(w->addr <= last && ptr < w->addr + w->size)
			break;
	if(!w)
		return;
	{
		uint32_t tag = ptr >> VM_TAG_SHIFT;
		if((tag == gEngine->tagCode || tag == gEngine->tagData || tag == gEngine->tagHeap) && w->threadId != Thread_GetId(t))
			return;
	}
	{
		char text[0x400], ctx[0x400];
		sprintf(text, MSG_WATCH_HIT, msg, w->name, (unsigned)w->addr, (int)w->size, (unsigned)ptr, (int)size);
		FormatErrorContext(ctx, text, t);
		MsgBox(ctx, "Assertion", OS_MB_ICONINFO | OS_MB_SYSTEMMODAL);
	}
}

// check the store of one element (size code sz, as in Opcode_Load) at ptr, then resolve the pointer
static void* WatchCheckStore(uint32_t ptr, uint32_t sz, Thread_t* t, const char* msg)
{
	static const uint32_t elemSize[3] = {1, 2, 4};
	WatchCheckRange2(ptr, elemSize[sz & 3], t, msg);
	return ResolvePtr(ptr, t);
}

// check a write of `size` bytes at ptr, then resolve the pointer
static void* WatchCheckRange(uint32_t ptr, uint32_t size, Thread_t* t, const char* msg)
{
	WatchCheckRange2(ptr, size, t, msg);
	return ResolvePtr(ptr, t);
}

// the checked variants of the eight writing instructions; same operands as the plain handlers

static int Opcode_StoreKeepWatch(Thread_t* t) // 09: ptr, v → v
{
	uint32_t v = Thread_Pop(t), ptr = Thread_Pop(t), sz = RdU8(t);
	StoreSized((uint8_t*)WatchCheckStore(ptr, sz, t, MSG_WATCH_WRITE), sz, v);
	Thread_Push(t, v);
	return 0;
}

static int Opcode_StoreWatch(Thread_t* t) // 0A: v, ptr →
{
	uint32_t ptr = Thread_Pop(t), v = Thread_Pop(t), sz = RdU8(t);
	StoreSized((uint8_t*)WatchCheckStore(ptr, sz, t, MSG_WATCH_WRITE), sz, v);
	return 0;
}

static int Opcode_StoreMultiWatch(Thread_t* t) // 0C: ptr, v[cnt] →
{
	static const uint32_t elemSize[3] = {1, 2, 4};
	uint32_t vals[0x100];
	uint32_t sz = RdU8(t), cnt = RdU8(t), i, ptr;
	for(i = 0; i < cnt; i++)
		vals[i] = Thread_Pop(t);
	ptr = Thread_Pop(t);
	for(i = cnt; i > 0; i--)
	{
		StoreSized((uint8_t*)WatchCheckStore(ptr, sz, t, MSG_WATCH_WRITE), sz, vals[i - 1]);
		ptr += elemSize[sz & 3];
	}
	return 0;
}

static int Opcode_MemcpyWatch(Thread_t* t) // 60: dst, src, n →
{
	uint32_t n = Thread_Pop(t);
	void* src = PopPtr(t);
	uint32_t dp = Thread_Pop(t);
	memcpy(WatchCheckRange(dp, n, t, MSG_WATCH_WRITE), src, n);
	return 0;
}

static int Opcode_MemclrWatch(Thread_t* t) // 61: dst, n →
{
	uint32_t n = Thread_Pop(t), dp = Thread_Pop(t);
	memset(WatchCheckRange(dp, n, t, MSG_WATCH_WRITE), 0, n);
	return 0;
}

static int Opcode_MemsetWatch(Thread_t* t) // 62: dst, n, val →
{
	uint32_t val = Thread_Pop(t), n = Thread_Pop(t), dp = Thread_Pop(t);
	memset(WatchCheckRange(dp, n, t, MSG_WATCH_WRITE), (int)(val & 0xff), n);
	return 0;
}

static int Opcode_StrcpyWatch(Thread_t* t) // 6A: dst, src →
{
	char* src = (char*)PopPtr(t);
	uint32_t dp = Thread_Pop(t);
	char* dst = (char*)WatchCheckRange(dp, (uint32_t)strlen(src) + 1, t, MSG_WATCH_WRITE);
	strcpy(dst, src);
	return 0;
}

// the check follows the write here: the length is only known once the text is formatted
static int Opcode_SprintfWatch(Thread_t* t) // 6F: args…, dst, fmt →
{
	char* fmt = (char*)PopPtr(t);
	uint32_t dp = Thread_Pop(t);
	char* dst = (char*)ResolvePtr(dp, t);
	VmSprintf(t, dst, fmt);
	WatchCheckRange2(dp, (uint32_t)strlen(dst) + 1, t, MSG_WATCH_WRITE);
	return 0;
}

// swap the eight checked handlers in and out of the main table
static void SetWatchMode(int on)
{
	if(on)
	{
		vm_optable_main[0x09] = Opcode_StoreKeepWatch;
		vm_optable_main[0x0a] = Opcode_StoreWatch;
		vm_optable_main[0x0c] = Opcode_StoreMultiWatch;
		vm_optable_main[0x60] = Opcode_MemcpyWatch;
		vm_optable_main[0x61] = Opcode_MemclrWatch;
		vm_optable_main[0x62] = Opcode_MemsetWatch;
		vm_optable_main[0x6a] = Opcode_StrcpyWatch;
		vm_optable_main[0x6f] = Opcode_SprintfWatch;
	}
	else
	{
		vm_optable_main[0x09] = Opcode_StoreKeep;
		vm_optable_main[0x0a] = Opcode_Store;
		vm_optable_main[0x0c] = Opcode_StoreMulti;
		vm_optable_main[0x60] = Opcode_Memcpy;
		vm_optable_main[0x61] = Opcode_Memclr;
		vm_optable_main[0x62] = Opcode_Memset;
		vm_optable_main[0x6a] = Opcode_Strcpy;
		vm_optable_main[0x6f] = Opcode_Sprintf;
	}
}

// turn the write checks on (non-zero) or off for every thread
static int Opcode_WatchMode(Thread_t* t) // 74: on →
{
	SetWatchMode(Thread_Pop(t) != 0);
	return 0;
}

// register a region to watch (WatchAdd); 0 when its size is 0
static int Opcode_WatchAdd(Thread_t* t) // 75: addr, size, name → ok
{
	char* name = (char*)PopPtr(t);
	uint32_t size = Thread_Pop(t), addr = Thread_Pop(t);
	Thread_Push(t, (uint32_t)WatchAdd(addr, size, name, t));
	return 0;
}

// ---- debugging message boxes -----------------------------------------------

/* a system-modal yes / no question box with the script's text; `flag`
 * selects the default button (yes when non-zero, no otherwise).  Pushes 1
 * when "yes" was chosen. */
static int Opcode_ConfirmBox(Thread_t* t) // 78: text, flag → yes
{
	uint32_t flag = Thread_Pop(t);
	char* text = (char*)PopPtr(t);
	uint32_t style = (flag ? 0 : OS_MB_DEFBUTTON2) | OS_MB_YESNO | OS_MB_ICONQUESTION | OS_MB_SYSTEMMODAL;
	Thread_Push(t, MsgBoxScript(text, MSG_CONFIRM, style) == OS_IDYES);
	return 0;
}

// a message box with the text and the thread's context (thread number, program, position)
static int Opcode_DebugMsg(Thread_t* t) // 79: text →
{
	char* text = (char*)PopPtr(t);
	char body[0x1000], ctx[0x1000];
	sprintf(body, "Message : %s", text);
	FormatErrorContext(ctx, body, t);
	MsgBoxScript(ctx, "Display", OS_MB_ICONINFO | OS_MB_SYSTEMMODAL);
	return 0;
}

// a message box with the value in decimal and hexadecimal and the thread's context
static int Opcode_DebugNum(Thread_t* t) // 7A: v →
{
	uint32_t v = Thread_Pop(t);
	char body[0x1000], ctx[0x1000];
	sprintf(body, "Number : %d ( $%.8x )", (int)v, (unsigned)v);
	FormatErrorContext(ctx, body, t);
	MsgBoxScript(ctx, "Assertion", OS_MB_ICONINFO | OS_MB_SYSTEMMODAL);
	return 0;
}

/* a message box with a hex and ASCII dump of `size` bytes at `mem` under
 * `title`, 16 bytes per line with control bytes shown as spaces; a size
 * outside 1 .. 0x400 is a script error */
static int Opcode_DebugDump(Thread_t* t) // 7B: title, mem, size →
{
	int32_t size = (int32_t)Thread_Pop(t);
	uint8_t* mem = (uint8_t*)PopPtr(t);
	char* title = (char*)PopPtr(t);
	char dump[0x4000], line[0x100], ascii[0x100], text[0x4000];
	int off;

	if(size < 1 || size > 0x400)
	{
		char msg[0x100];
		sprintf(msg, MSG_BAD_DUMP_SIZE, (int)size);
		ScriptError(msg, t);
	}
	dump[0] = 0;
	for(off = 0; off < size; off += 16)
	{
		int n = size - off < 16 ? size - off : 16, i;
		line[0] = 0;
		for(i = 0; i < n; i++)
		{
			char tmp[0x100];
			sprintf(tmp, "%s%.2X ", line, mem[off + i]);
			strcpy(line, tmp);
		}
		memcpy(ascii, mem + off, (size_t)n);
		ascii[n] = 0;
		for(i = 0; i < n; i++)
			if((uint8_t)ascii[i] < 0x20)
				ascii[i] = ' ';
		sprintf(text, "%s\n0x%.4X : %s %s", dump, off, line, ascii);
		strcpy(dump, text);
	}
	sprintf(text, "%s\n\n%s", title, dump);
	MsgBoxScript(text, "memory dump", OS_MB_SYSTEMMODAL);
	return 0;
}

/* render a hex dump of `len` bytes at `data` into the managed bitmap `bmp`
 * at (x, y) with the given font size and colour (Vm_DebugText) */
static int Opcode_DebugText(Thread_t* t) // 7D: bmp, x, y, data, len, fontSize, colour →
{
	uint32_t colour = Thread_Pop(t), fontSize = Thread_Pop(t), len = Thread_Pop(t);
	const void* data = PopPtr(t);
	int32_t y = (int32_t)Thread_Pop(t), x = (int32_t)Thread_Pop(t);
	uint32_t bmp = Thread_Pop(t);
	Vm_DebugText(bmp, x, y, data, len, (int)fontSize, colour, t);
	return 0;
}

/* "7F" is a build-dependent debugging hook before 1.667, a no-op in most
 * builds; from 1.667 on the byte dispatches the "7F xx" family (Opcode_Sys7F) */
static int Opcode_Nop(Thread_t* t) // 7F (before 1.667): →
{
	BGI_UNUSED(t);
	return 0;
}

// ---- sub-table dispatchers --------------------------------------------------

/*
 * Fill one family's 256-entry handler table for the selected engine
 * version.  `fam` names the family in the profile's opcode sets: an opcode
 * the version does not define stays NULL, which raises the original's
 * "undefined instruction" error when a script reaches it; one it defines but
 * no registration covers gets Opcode_NotImplemented.  `list` holds the `n`
 * registrations: each carries the generations it is valid for, and when
 * several cover the opcode for this version the one with the narrowest
 * range wins, so a version-specific variant can sit next to the general
 * handler.  A per-opcode override from the profile (Engine_OpcodeOverride)
 * replaces the opcode set's answer.
 */
void Vm_FillTable(VmHandler_t* table, OpFamily_t fam, const OpEntry_t* list, size_t n)
{
	int op;
	size_t i;
	EngineGen_t gen = gEngine->gen;
	for(op = 0; op < 256; op++)
	{
		int defined = OpSet_Has(fam, op, gen);
		int ov = Engine_OpcodeOverride(fam, op);
		const OpEntry_t* best = NULL;
		if(ov >= 0)
			defined = ov;
		table[op] = NULL;
		if(!defined)
			continue;
		for(i = 0; i < n; i++)
		{
			const OpEntry_t* e = &list[i];
			if(e->op != op || gen < e->from || gen > e->to)
				continue;
			if(!best || e->to - e->from < best->to - best->from)
				best = e; // the most specific range wins
		}
		table[op] = best ? best->fn : Opcode_NotImplemented;
	}
}

/* the stand-in for an opcode the profile defines but no handler covers: a
 * script error naming the opcode bytes (two for a family instruction) and
 * the engine version */
int Opcode_NotImplemented(Thread_t* t)
{
	uint32_t ip = Thread_CurOpIP(t);
	uint8_t op = t->code[ip];
	char msg[0x100];
	if(op >= 0x7f && op != 0xff)
		sprintf(msg, "Opcode $%02X%02X of engine version %s is not implemented yet", op, t->code[ip + 1], gEngine->name);
	else
		sprintf(msg, "Opcode $%02X of engine version %s is not implemented yet", op, gEngine->name);
	ScriptError(msg, t);
	return 0;
}

/* --list-missing: print, family by family, the opcodes the profile defines
 * that still run Opcode_NotImplemented, then their total */
void Vm_ListMissing(FILE* out)
{
	static const struct
	{
		const char* name;
		VmHandler_t* table;
	} fams[] = {{"main", vm_optable_main}, {"7F", vm_optable_7F}, {"80", vm_optable_80}, {"81", vm_optable_81},
		{"90", vm_optable_90}, {"91", vm_optable_91}, {"92", vm_optable_92}, {"A0", vm_optable_A0},
		{"B0", vm_optable_B0}, {"C0", vm_optable_C0}, {"D0", vm_optable_D0}, {"E0", vm_optable_E0},
		{"EF", vm_optable_EF}};
	size_t f;
	int total = 0;
	for(f = 0; f < BGI_COUNTOF(fams); f++)
	{
		int op, n = 0;
		for(op = 0; op < 256; op++)
		{
			if(fams[f].table[op] != Opcode_NotImplemented)
				continue;
			if(n++ == 0)
				fprintf(out, "%s:", fams[f].name);
			fprintf(out, " %02X", op);
		}
		if(n)
			fprintf(out, "\n");
		total += n;
	}
	fprintf(out, "%s: %d opcodes without a handler\n", gEngine->name, total);
}

// fill every handler table for the selected engine profile; once, before the first instruction
void Vm_TablesInit(void)
{
	Vm_OptableMainInit();
	Vm_Optable80Init();
	Vm_Optable90Init();
	Vm_Optable91Init();
	Vm_Optable92Init();
	Vm_OptableA0Init();
	Vm_OptableB0Init();
	Vm_OptableC0Init();
	// the families of the later builds (docs/versions.md): no handlers yet
	Vm_FillTable(vm_optable_7F, OPFAM_7F, NULL, 0);
	Vm_Optable81Init();
	Vm_FillTable(vm_optable_D0, OPFAM_D0, NULL, 0);
	Vm_FillTable(vm_optable_E0, OPFAM_E0, NULL, 0);
	Vm_OptableEFInit(); // OpenBGI's own extensions
}

// the tables of the families that later builds added (docs/versions.md)
VmHandler_t vm_optable_7F[256]; // "7F xx", 1.667 on
VmHandler_t vm_optable_81[256]; // "81 xx", 1.69 build 472 on (an empty family until 1.529; ops_sys81.c)
VmHandler_t vm_optable_D0[256]; // "D0 xx", 1.535 on: the evaluation instructions
VmHandler_t vm_optable_E0[256]; // "E0 xx", 1.588 on (its own table from 1.653)
VmHandler_t vm_optable_EF[256]; // "EF xx": the OpenBGI extensions, a byte no original build uses (ops_obgi.c)

/* read the second opcode byte and dispatch through the family table; a
 * sub-opcode without a handler raises the family's "undefined instruction"
 * script error (`errFmt` takes the byte) */
static int Dispatch(Thread_t* t, VmHandler_t* table, const char* errFmt)
{
	uint8_t sub = RdU8(t);
	if(!table[sub])
	{
		char msg[0x100];
		sprintf(msg, errFmt, sub);
		ScriptError(msg, t);
	}
	return table[sub](t);
}

// the dispatchers of the two-byte families; each reads the sub-opcode and runs its handler

static int Opcode_Sys(Thread_t* t) // 80: the "80 xx" system family (ops_sys.c)
{
	return Dispatch(t, vm_optable_80, MSG_UNDEF_SYS);
}

static int Opcode_Grp0(Thread_t* t) // 90: the "90 xx" graphics family (ops_gfx0.c)
{
	return Dispatch(t, vm_optable_90, MSG_UNDEF_GFX90);
}

static int Opcode_Grp1(Thread_t* t) // 91: the "91 xx" graphics family (ops_gfx1.c)
{
	return Dispatch(t, vm_optable_91, MSG_UNDEF_GFX91);
}

static int Opcode_Grp2(Thread_t* t) // 92: the "92 xx" graphics family (ops_gfx2.c)
{
	return Dispatch(t, vm_optable_92, MSG_UNDEF_GFX92);
}

static int Opcode_Snd(Thread_t* t) // A0: the "A0 xx" sound family (ops_snd.c)
{
	return Dispatch(t, vm_optable_A0, MSG_UNDEF_SND);
}

static int Opcode_Ext0(Thread_t* t) // B0: the "B0 xx" extension family (ops_ext0.c)
{
	return Dispatch(t, vm_optable_B0, MSG_UNDEF_EXTB0);
}

static int Opcode_Ext1(Thread_t* t) // C0: the "C0 xx" extension family (ops_ext1.c)
{
	return Dispatch(t, vm_optable_C0, MSG_UNDEF_EXTC0);
}

// the dispatchers of the later builds' families
static int Opcode_Sys7F(Thread_t* t) // 7F (1.667 on): the "7F xx" family
{
	return Dispatch(t, vm_optable_7F, MSG_UNDEF_SYS7F);
}

static int Opcode_Sys81(Thread_t* t) // 81 (1.69/472 on): the "81 xx" system family (ops_sys81.c)
{
	return Dispatch(t, vm_optable_81, MSG_UNDEF_SYS81);
}

static int Opcode_Eval(Thread_t* t) // D0 (1.535 on): the "D0 xx" evaluation family
{
	return Dispatch(t, vm_optable_D0, MSG_UNDEF_EVAL);
}

static int Opcode_Ext2(Thread_t* t) // E0 (1.588 on): the "E0 xx" extension family
{
	return Dispatch(t, vm_optable_E0, MSG_UNDEF_EXTE0);
}

static int Opcode_Obgi(Thread_t* t) // EF: the "EF xx" OpenBGI extension family (ops_obgi.c)
{
	return Dispatch(t, vm_optable_EF, MSG_UNDEF_OBGI);
}

int Opcode_User(Thread_t* t); // FF: the user-defined instructions "FF xx" (userop.c)

// ---- the main table -----------------------------------------------------

VmHandler_t vm_optable_main[256]; // the base instructions and the family dispatchers; SetWatchMode patches it

/* register the base instructions and the family dispatchers.  A variant for
 * later generations follows the general entry of its opcode ("6C"); the
 * dispatchers of the families that later builds added come last. */
void Vm_OptableMainInit(void)
{
	static const OpEntry_t ops[] = {
		{0x00, Opcode_PushI8, GEN_FIRST, GEN_LAST},
		{0x01, Opcode_PushI16, GEN_FIRST, GEN_LAST},
		{0x02, Opcode_PushI32, GEN_FIRST, GEN_LAST},
		{0x04, Opcode_PushLocalAddr, GEN_FIRST, GEN_LAST},
		{0x05, Opcode_PushCodeAddr, GEN_FIRST, GEN_LAST},
		{0x06, Opcode_PushCodeOff, GEN_FIRST, GEN_LAST},
		{0x08, Opcode_Load, GEN_FIRST, GEN_LAST},
		{0x09, Opcode_StoreKeep, GEN_FIRST, GEN_LAST},
		{0x0a, Opcode_Store, GEN_FIRST, GEN_LAST},
		{0x0b, Opcode_StoreInline, GEN_FIRST, GEN_LAST},
		{0x0c, Opcode_StoreMulti, GEN_FIRST, GEN_LAST},
		{0x10, Opcode_PushFp, GEN_FIRST, GEN_LAST},
		{0x11, Opcode_SetFp, GEN_FIRST, GEN_LAST},
		{0x14, Opcode_Jmp, GEN_FIRST, GEN_LAST},
		{0x15, Opcode_Jcc, GEN_FIRST, GEN_LAST},
		{0x16, Opcode_Call, GEN_FIRST, GEN_LAST},
		{0x17, Opcode_Ret, GEN_FIRST, GEN_LAST},
		{0x20, Opcode_Add, GEN_FIRST, GEN_LAST},
		{0x21, Opcode_Sub, GEN_FIRST, GEN_LAST},
		{0x22, Opcode_Mul, GEN_FIRST, GEN_LAST},
		{0x23, Opcode_Div, GEN_FIRST, GEN_LAST},
		{0x24, Opcode_Mod, GEN_FIRST, GEN_LAST},
		{0x25, Opcode_And, GEN_FIRST, GEN_LAST},
		{0x26, Opcode_Or, GEN_FIRST, GEN_LAST},
		{0x27, Opcode_Xor, GEN_FIRST, GEN_LAST},
		{0x28, Opcode_Not, GEN_FIRST, GEN_LAST},
		{0x29, Opcode_Shl, GEN_FIRST, GEN_LAST},
		{0x2a, Opcode_Shr, GEN_FIRST, GEN_LAST},
		{0x2b, Opcode_Sar, GEN_FIRST, GEN_LAST},
		{0x30, Opcode_Eq, GEN_FIRST, GEN_LAST},
		{0x31, Opcode_Ne, GEN_FIRST, GEN_LAST},
		{0x32, Opcode_Le, GEN_FIRST, GEN_LAST},
		{0x33, Opcode_Ge, GEN_FIRST, GEN_LAST},
		{0x34, Opcode_Lt, GEN_FIRST, GEN_LAST},
		{0x35, Opcode_Gt, GEN_FIRST, GEN_LAST},
		{0x38, Opcode_Land, GEN_FIRST, GEN_LAST},
		{0x39, Opcode_Lor, GEN_FIRST, GEN_LAST},
		{0x3a, Opcode_Lnot, GEN_FIRST, GEN_LAST},
		{0x40, Opcode_Select, GEN_FIRST, GEN_LAST},
		{0x42, Opcode_Muldiv, GEN_FIRST, GEN_LAST},
		{0x43, Opcode_Atan2, GEN_1_69_472, GEN_LAST},
		{0x48, Opcode_Sin, GEN_FIRST, GEN_LAST},
		{0x49, Opcode_Cos, GEN_FIRST, GEN_LAST},
		{0x50, Opcode_Add64, GEN_1_69_472, GEN_LAST},
		{0x51, Opcode_Sub64, GEN_1_69_472, GEN_LAST},
		{0x52, Opcode_Mul64, GEN_1_69_472, GEN_LAST},
		{0x53, Opcode_Div64, GEN_1_69_472, GEN_LAST},
		{0x54, Opcode_Mod64, GEN_1_69_472, GEN_LAST},
		{0x60, Opcode_Memcpy, GEN_FIRST, GEN_LAST},
		{0x61, Opcode_Memclr, GEN_FIRST, GEN_LAST},
		{0x62, Opcode_Memset, GEN_FIRST, GEN_LAST},
		{0x63, Opcode_Memcmp, GEN_FIRST, GEN_LAST},
		{0x66, Opcode_Strstr, GEN_1_529, GEN_LAST},
		{0x67, Opcode_Strreplace, GEN_FIRST, GEN_LAST},
		{0x68, Opcode_Strlen, GEN_FIRST, GEN_LAST},
		{0x69, Opcode_Streq, GEN_FIRST, GEN_LAST},
		{0x6a, Opcode_Strcpy, GEN_FIRST, GEN_LAST},
		{0x6b, Opcode_Strcat2, GEN_FIRST, GEN_LAST},
		{0x6c, Opcode_Getchar, GEN_FIRST, GEN_LAST},
		{0x6c, Opcode_Getchar_653, GEN_1_653, GEN_LAST},
		{0x6d, Opcode_Strlwr, GEN_FIRST, GEN_LAST},
		{0x6e, Opcode_Strquote, GEN_FIRST, GEN_LAST},
		{0x6f, Opcode_Sprintf, GEN_FIRST, GEN_LAST},
		{0x70, Opcode_Lalloc, GEN_FIRST, GEN_LAST},
		{0x71, Opcode_Lfree, GEN_FIRST, GEN_LAST},
		{0x74, Opcode_WatchMode, GEN_FIRST, GEN_LAST},
		{0x75, Opcode_WatchAdd, GEN_FIRST, GEN_LAST},
		{0x78, Opcode_ConfirmBox, GEN_FIRST, GEN_LAST},
		{0x79, Opcode_DebugMsg, GEN_FIRST, GEN_LAST},
		{0x7a, Opcode_DebugNum, GEN_FIRST, GEN_LAST},
		{0x7b, Opcode_DebugDump, GEN_FIRST, GEN_LAST},
		{0x7d, Opcode_DebugText, GEN_FIRST, GEN_LAST},
		{0x7e, Opcode_ClipboardSet, GEN_1_69_472, GEN_LAST},
		{0x7f, Opcode_Nop, GEN_FIRST, GEN_1_662}, // a build-dependent debugging hook before 1.667
		{0x80, Opcode_Sys, GEN_FIRST, GEN_LAST},
		{0x90, Opcode_Grp0, GEN_FIRST, GEN_LAST},
		{0x91, Opcode_Grp1, GEN_FIRST, GEN_LAST},
		{0x92, Opcode_Grp2, GEN_FIRST, GEN_LAST},
		{0xa0, Opcode_Snd, GEN_FIRST, GEN_LAST},
		{0xb0, Opcode_Ext0, GEN_FIRST, GEN_LAST},
		{0xc0, Opcode_Ext1, GEN_FIRST, GEN_LAST},
		{0xff, Opcode_User, GEN_FIRST, GEN_LAST},
		{0x7f, Opcode_Sys7F, GEN_1_667, GEN_LAST},
		{0x81, Opcode_Sys81, GEN_1_69_451, GEN_LAST},
		{0xd0, Opcode_Eval, GEN_1_535, GEN_LAST},
		{0xe0, Opcode_Ext2, GEN_1_588, GEN_LAST},
		{0xef, Opcode_Obgi, GEN_FIRST, GEN_LAST},
	};
	Vm_FillTable(vm_optable_main, OPFAM_MAIN, ops, BGI_COUNTOF(ops));
}
