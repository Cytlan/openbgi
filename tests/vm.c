/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/vm.c - unit test of the base instruction set of the script VM
 *              (src/vm/ops_base.c and src/vm/thread.c, declared in
 *              src/include/bgi/vm.h); built as test_vm by `make test`
 *
 * Small programs are assembled into a thread's code area and run through
 * the dispatcher exactly as the scheduler does it; the evaluation stack,
 * the data area and the local heap are then inspected.  Script errors
 * are caught with the engine's own try frames (the error box goes to
 * stderr with BGI_MSGBOX_STDERR, which this test sets).  Opcodes are
 * written as their byte values; the mnemonic of each is in the comments
 * and in src/vm/ops_base.c.
 *
 * The groups of tests, in the order they run:
 *
 *   push         - "00" .. "02" with sign extension, the tagged local
 *                  address of "04" and the code addresses of "05" / "06"
 *                  relative to the opcode
 *   arithmetic   - every binary operator "20" .. "39" on chosen operands
 *                  (truncating division, division by zero, masked shift
 *                  counts, signed comparisons), not / lnot / select,
 *                  muldiv with its 64-bit intermediate, sin / cos in 16.16
 *   memory       - load and store in the three sizes with sign extension,
 *                  store-keep, inline bytes, store-multi, and memset /
 *                  memcpy / memcmp / memclr between locals
 *   strings      - the string instructions "68" .. "6F" on code and data
 *                  area strings, including the character classifier "6C"
 *                  and the argument order of "6F"
 *   control flow - jcc taken and not taken, call / ret through the frame
 *                  stack, ret at the outermost frame ending the thread
 *   heap         - lalloc / lfree: tagged, non-overlapping blocks
 *   errors       - a jump to NULL raises a script error; an undefined
 *                  opcode has no handler
 *   extensions   - the "EF xx" family of OpenBGI's own instructions exists
 *                  in the profile, reports its level and sets the text
 *                  switches it is there for
 *   later builds - the services that instructions of the later builds rely
 *                  on: the priority limit by generation, the kept error of
 *                  "81 6A" / "81 6B", the gamepad table of "81 1B" and the
 *                  clock settings of "81 68" / "81 04"
 *   borrowed areas - the threads of "81 44" in parts of another thread's
 *                  code and data areas: where the parts go, what the
 *                  lender keeps, frames and modules of a borrower, the
 *                  order in which they end, the instruction itself
 *   files        - "80 27" moving a file, the file accessor "81 28" ..
 *                  "81 2B" in both of its generations, the result codes
 *                  of the file dialog "81 38"
 *   launcher     - the instructions a launcher needs ("EF 2x"): a
 *                  folder's listing, the settings and what the engine says
 *                  of the host through them, no game where there is none
 *   program file - "81 3F" and what a program makes of the engine's own
 *                  program file (bgi/follow.h): the file there from then
 *                  on, a number computed from its bytes equal to any
 *                  constant but 0 and -1, two such numbers compared as
 *                  they are, the marks gone with what an engine service
 *                  writes over them and with a freed block, a variable of
 *                  the global memory marked again in the next run; "47"
 *                  of 1.492
 *
 * A failure means an instruction computes, pushes or pops differently
 * from the original, which every script would notice.
 */
#include "bgi/vm.h"
#include "bgi/core.h"
#include "bgi/host.h"
#include "bgi/sys.h"
#include "bgi/error.h"
#include "bgi/strutil.h"
#include "bgi/input.h"
#include "bgi/gfx/text.h"
#include "host/os.h"
#include "bgi/version.h"
#include "bgi/file.h"
#include "bgi/follow.h"

#include "testhost.h"

#include <stdio.h>
#include <math.h>

#include "check.h"

// ---- a tiny assembler ------------------------------------------------------

// a program under construction: raw bytes appended by the helpers below
typedef struct Asm
{
	uint8_t code[0x1000]; // the bytes
	uint32_t len;         // how many are used
} Asm_t;

// append a byte
static void A8(Asm_t* a, uint32_t v)
{
	a->code[a->len++] = (uint8_t)v;
}

// append a little-endian 16-bit value
static void A16(Asm_t* a, uint32_t v)
{
	A8(a, v & 0xff);
	A8(a, v >> 8);
}

// append a little-endian 32-bit value
static void A32(Asm_t* a, uint32_t v)
{
	A16(a, v & 0xffff);
	A16(a, v >> 16);
}

static void PushI8(Asm_t* a, int v) // 00
{
	A8(a, 0x00);
	A8(a, (uint32_t)v);
}

static void PushI16(Asm_t* a, int v) // 01
{
	A8(a, 0x01);
	A16(a, (uint32_t)v);
}

static void PushI32(Asm_t* a, uint32_t v) // 02
{
	A8(a, 0x02);
	A32(a, v);
}

// append a bare opcode
static void Op(Asm_t* a, uint32_t op)
{
	A8(a, op);
}

// 17 with the frame pointer reset: a return at fp 0 ends the program
static void Ret(Asm_t* a)
{
	PushI32(a, 0);
	A8(a, 0x11);
	A8(a, 0x17);
}

// ---- running ------------------------------------------------------------------

static Thread_t* gT; // the one thread every program runs on (made by main)

/* What the scheduler and the host do after an instruction that asked the
 * host (bgi/request.h): the requests are answered by the OS layer as the
 * program's host answers them (the boxes go to stderr and take their
 * default button, the dialogs are cancelled: there is no display), and
 * the wait an instruction holds the engine with is polled until it is
 * over - then the instruction is done and the program goes on (0).  Any
 * other code is returned as it is. */
static int Settle(OpenBGI_t* o, Thread_t* t, int rc)
{
	int polls = 0;
	OS_AnswerRequests(o);
	while(rc != 0 && o->vm.holder == t && (Thread_GetFlags(t) & THR_FLAG_WAITING) && polls++ < 100000)
	{
		int w = Thread_PollWait(o, t);
		OS_AnswerRequests(o);
		if(w != 0)
			return w == 1 ? 0 : 1;
	}
	return rc;
}

/* Load the program into the thread's code area and step it from offset 0
 * with the frame pointer and stack reset, dispatching through
 * o->vm.optablemain as the scheduler does, until a handler returns a
 * non-zero scheduler code (4: the thread ended) or 100000 steps have
 * passed.  Returns that code, or -1 when an opcode has no handler. */
static int Run(OpenBGI_t* o, const Asm_t* a)
{
	int rc = 0, steps = 0;
	Thread_t* t = gT;
	memcpy(t->code, a->code, a->len);
	t->codeUsed = a->len;
	Thread_SetIP(t, 0);
	Thread_SetFP(t, 0);
	t->sp = 0;
	while(rc == 0 && steps++ < 100000)
	{
		uint8_t op = Thread_FetchOp(t);
		if(!o->vm.optablemain[op])
			return -1; // the scheduler raises MSG_UNDEFINED_OP here
		rc = Settle(o, t, o->vm.optablemain[op](o, t));
	}
	return rc;
}

/* the value on top of the stack, without popping (the stack wraps: on an
 * empty stack this reads the last entry) */
static uint32_t Top(void)
{
	Thread_t* t = gT;
	uint32_t i = t->sp ? t->sp - 1 : t->stackEntries - 1;
	return t->stack[i];
}

// the number of values on the evaluation stack
static uint32_t Depth(void)
{
	return gT->sp;
}

/* append the final return, run the program (which has to end normally with exactly one
 * value on the stack) and return that value */
static uint32_t Eval(OpenBGI_t* o, Asm_t* a)
{
	Ret(a);
	CHECK(Run(o, a) == 4);
	CHECK(Depth() == 1);
	return Top();
}

// ---- the tests -----------------------------------------------------------------

/* The push instructions: "00" and "01" sign-extend their immediate, "02"
 * pushes it as is; "04" pushes fp - n with the data tag; "05" pushes the
 * code address opcode offset + n with the code tag and "06" the untagged
 * offset, both relative to the opcode's own position. */
static void TestPush(OpenBGI_t* o)
{
	Asm_t a;
	printf("push\n");
	a.len = 0;
	PushI8(&a, -5);
	CHECK(Eval(o, &a) == 0xfffffffbu); // sign extended
	a.len = 0;
	PushI16(&a, 0x8000);
	CHECK(Eval(o, &a) == 0xffff8000u);
	a.len = 0;
	PushI32(&a, 0x12345678);
	CHECK(Eval(o, &a) == 0x12345678u);
	// 04: a local's address is fp - n with the data tag; 05 / 06 are relative to the
	// opcode
	a.len = 0;
	Op(&a, 0x04);
	A16(&a, 8);
	CHECK(Eval(o, &a) == ((0u - 8u) | VM_TAG_DATA(o)));
	a.len = 0;
	Op(&a, 0x05);
	A16(&a, 0x10); // code address: opIP (0) + 0x10, tagged
	CHECK(Eval(o, &a) == (0x10u | VM_TAG_CODE(o)));
	a.len = 0;
	Op(&a, 0x7f); // nop first, so the opcode is at 1
	Op(&a, 0x06);
	A16(&a, 0xfffe); // -2 -> 1 - 2
	CHECK(Eval(o, &a) == 0xffffffffu);
}

/* The arithmetic, logic and comparison instructions on a table of operand
 * pairs (a pushed first, then b, so b is on top): the quirks a script
 * relies on are division truncating toward zero and answering -1 for a
 * zero divisor, shift counts masked to 5 bits, a logical "2A" and an
 * arithmetic "2B" right shift, and signed comparisons.  Then the unary
 * not and lnot, select with a true and a false condition, muldiv keeping
 * a 64-bit intermediate, and sin / cos of 16.16 degrees giving 16.16
 * results (cos 60 may land on either side of 0.5 after truncation). */
static void TestArith(OpenBGI_t* o)
{
	static const struct
	{
		uint32_t op, a, b, r; // the opcode, both operands and the expected result
	} cases[] = {
		{0x20, 7, 5, 12},
		{0x21, 7, 9, 0xfffffffeu},
		{0x22, 0xffffffffu, 3, 0xfffffffdu},
		{0x23, 0xfffffff9u, 2, 0xfffffffdu}, // -7 / 2 = -3 (truncation)
		{0x23, 5, 0, 0xffffffffu},           // division by zero: -1
		{0x24, 0xfffffff9u, 2, 0xffffffffu}, // -7 % 2 = -1
		{0x24, 5, 0, 0xffffffffu},
		{0x25, 0xf0f0, 0xff00, 0xf000},
		{0x26, 0xf0f0, 0x0f0f, 0xffff},
		{0x27, 0xf0f0, 0xffff, 0x0f0f},
		{0x29, 1, 35, 8},                     // the shift count is masked to 5 bits
		{0x2a, 0x80000000u, 31, 1},           // logical right shift
		{0x2b, 0x80000000u, 31, 0xffffffffu}, // arithmetic right shift
		{0x30, 3, 3, 1},
		{0x31, 3, 4, 1},
		{0x32, 0xffffffffu, 0, 1}, // -1 <= 0, signed
		{0x33, 0xffffffffu, 0, 0},
		{0x34, 0xffffffffu, 0, 1},
		{0x35, 0x7fffffffu, 0x80000000u, 1}, // INT_MAX > INT_MIN
		{0x38, 2, 0, 0},
		{0x39, 0, 2, 1},
	};
	size_t i;
	Asm_t a;
	printf("arithmetic\n");
	for(i = 0; i < sizeof cases / sizeof cases[0]; i++)
	{
		uint32_t r;
		a.len = 0;
		PushI32(&a, cases[i].a);
		PushI32(&a, cases[i].b);
		Op(&a, cases[i].op);
		r = Eval(o, &a);
		if(r != cases[i].r)
			printf("  op %02x: %08x, %08x -> %08x, expected %08x\n", cases[i].op, cases[i].a, cases[i].b, r, cases[i].r);
		CHECK(r == cases[i].r);
	}
	// unary and ternary
	a.len = 0;
	PushI32(&a, 0x0f0f0f0f);
	Op(&a, 0x28);
	CHECK(Eval(o, &a) == 0xf0f0f0f0u);
	a.len = 0;
	PushI32(&a, 0);
	Op(&a, 0x3a);
	CHECK(Eval(o, &a) == 1);
	a.len = 0;
	PushI32(&a, 1);  // c
	PushI32(&a, 10); // tv
	PushI32(&a, 20); // f
	Op(&a, 0x40);
	CHECK(Eval(o, &a) == 10);
	a.len = 0;
	PushI32(&a, 0);
	PushI32(&a, 10);
	PushI32(&a, 20);
	Op(&a, 0x40);
	CHECK(Eval(o, &a) == 20);
	// muldiv with a 64-bit intermediate: 0x10000 * 0x10000 / 0x100
	a.len = 0;
	PushI32(&a, 0x10000);
	PushI32(&a, 0x10000);
	PushI32(&a, 0x100);
	Op(&a, 0x42);
	CHECK(Eval(o, &a) == 0x1000000);
	// sin / cos of 16.16 degrees in 16.16
	a.len = 0;
	PushI32(&a, 90 << 16);
	Op(&a, 0x48);
	CHECK(Eval(o, &a) == 0x10000);
	a.len = 0;
	PushI32(&a, 60 << 16);
	Op(&a, 0x49);
	{
		uint32_t c = Eval(o, &a); // cos 60 = 0.5, truncated towards zero from the double
		CHECK(c == 0x8000 || c == 0x7fff);
	}
	a.len = 0;
	PushI32(&a, (uint32_t) - (90 << 16));
	Op(&a, 0x48);
	CHECK(Eval(o, &a) == 0xffff0000u);
}

/* The memory instructions with the frame pointer set so that locals land
 * at known data offsets: "0A" stores a dword and "08" loads it back as a
 * sign-extended byte, a sign-extended word and a dword; "09" stores and
 * keeps the value on the stack; "0B" copies the inline bytes that follow
 * the opcode; "0C" stores several values, the first pushed at the lowest
 * address; "60" .. "63" are memcpy, memclr, memset (low byte only) and
 * memcmp (1 when equal) between locals.  The data area is inspected
 * directly after each program. */
static void TestMemory(OpenBGI_t* o)
{
	Asm_t a;
	Thread_t* t = gT;
	printf("memory\n");
	// store a dword at data[0x20] (fp = 0x40, local address fp - 0x20) and load it back
	// in each size
	a.len = 0;
	PushI32(&a, 0x40);
	Op(&a, 0x11); // set fp
	PushI32(&a, 0x8081fffe);
	Op(&a, 0x04);
	A16(&a, 0x20);
	Op(&a, 0x0a); // store: the pointer on top, the value under it
	A8(&a, 2);
	Op(&a, 0x04);
	A16(&a, 0x20);
	Op(&a, 0x08);
	A8(&a, 0); // s8: 0xfe -> -2
	Ret(&a);
	CHECK(Run(o, &a) == 4);
	CHECK(Top() == 0xfffffffeu);
	CHECK(memcmp(t->data + 0x20, "\xfe\xff\x81\x80", 4) == 0);
	a.len = 0;
	PushI32(&a, 0x40);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x20);
	Op(&a, 0x08);
	A8(&a, 1); // s16: 0xfffe -> -2
	CHECK(Eval(o, &a) == 0xfffffffeu);
	a.len = 0;
	PushI32(&a, 0x40);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x20);
	Op(&a, 0x08);
	A8(&a, 2); // dword
	CHECK(Eval(o, &a) == 0x8081fffeu);

	// 09 keeps the value; 0B copies inline bytes; 0C stores several values (first pushed
	// lowest)
	a.len = 0;
	PushI32(&a, 0x40);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x10); // local address fp - 0x10 = data[0x30]
	PushI32(&a, 0x1234);
	Op(&a, 0x09);
	A8(&a, 1); // a word
	CHECK(Eval(o, &a) == 0x1234);
	CHECK(memcmp(t->data + 0x30, "\x34\x12", 2) == 0);
	a.len = 0;
	PushI32(&a, 0x40);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x10);
	Op(&a, 0x0b);
	A8(&a, 3); // three inline bytes follow
	A8(&a, 'a');
	A8(&a, 'b');
	A8(&a, 0);
	Ret(&a);
	CHECK(Run(o, &a) == 4);
	CHECK(strcmp((char*)t->data + 0x30, "ab") == 0);
	a.len = 0;
	PushI32(&a, 0x40);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x10);
	PushI32(&a, 1);
	PushI32(&a, 2);
	PushI32(&a, 3);
	Op(&a, 0x0c);
	A8(&a, 2); // dwords
	A8(&a, 3); // three of them
	Ret(&a);
	CHECK(Run(o, &a) == 4);
	CHECK(memcmp(t->data + 0x30, "\x01\0\0\0\x02\0\0\0\x03\0\0\0", 12) == 0);

	// memset / memcpy / memcmp / memclr between locals (fp = 0x80)
	a.len = 0;
	PushI32(&a, 0x80);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x40); // dst = data[0x40]
	PushI32(&a, 8);
	PushI32(&a, 0x1ab); // only the low byte
	Op(&a, 0x62);
	Op(&a, 0x04);
	A16(&a, 0x20); // dst = data[0x60]
	Op(&a, 0x04);
	A16(&a, 0x40); // src = data[0x40]
	PushI32(&a, 8);
	Op(&a, 0x60);
	Op(&a, 0x04);
	A16(&a, 0x40);
	Op(&a, 0x04);
	A16(&a, 0x20);
	PushI32(&a, 8);
	Op(&a, 0x63); // equal -> 1
	Ret(&a);
	CHECK(Run(o, &a) == 4);
	CHECK(Top() == 1);
	CHECK(memcmp(t->data + 0x40, "\xab\xab\xab\xab\xab\xab\xab\xab", 8) == 0);
	CHECK(memcmp(t->data + 0x60, "\xab\xab\xab\xab\xab\xab\xab\xab", 8) == 0);
	a.len = 0;
	PushI32(&a, 0x80);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x40);
	PushI32(&a, 4);
	Op(&a, 0x61); // memclr: the first four bytes only
	Ret(&a);
	CHECK(Run(o, &a) == 4);
	CHECK(memcmp(t->data + 0x40, "\0\0\0\0\xab\xab\xab\xab", 8) == 0);
}

/* The string instructions: "6A" strcpy from a literal in the code area
 * (reached through a tagged code address) and "68" strlen; "69" streq,
 * "6D" strlwr in place, "6B" the concatenation of two strings into a
 * third and "6E" the quoting with a given character, all on data-area
 * strings; "6C" pushes a character's code, whether it is double-byte and
 * whether it is a kinsoku character (one that may not start a line);
 * "6F" sprintf, whose arguments are popped in order of appearance after
 * the format and the destination, so a script pushes them in reverse.
 * The destination may also be a source (`strcat(buf, buf, "x")` appends),
 * as the scripts rely on. */
static void TestStrings(OpenBGI_t* o)
{
	Asm_t a;
	Thread_t* t = gT;
	uint32_t strAt;
	printf("strings\n");
	// a literal in the code area, reached through a tagged code address
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x80); // dst = data[0x80]
	Op(&a, 0x05);
	A16(&a, 0); // placeholder: patched to point at the literal
	strAt = a.len - 2;
	Op(&a, 0x6a); // strcpy
	Op(&a, 0x04);
	A16(&a, 0x80);
	Op(&a, 0x68); // strlen
	Ret(&a);
	{ // the literal after the code; the operand of "05" is relative to the opcode's own offset
		uint32_t lit = a.len, opAt = strAt - 1;
		memcpy(a.code + a.len, "Hello, BGI", 11);
		a.len += 11;
		a.code[strAt] = (uint8_t)(lit - opAt);
		a.code[strAt + 1] = (uint8_t)((lit - opAt) >> 8);
	}
	CHECK(Run(o, &a) == 4);
	CHECK(Top() == 10);
	CHECK(strcmp((char*)t->data + 0x80, "Hello, BGI") == 0);

	// streq, strcat2, strlwr, strquote on data-area strings (fp = 0x100: local 0x80 is
	// data[0x80], local 0x70 data[0x90])
	strcpy((char*)t->data + 0x80, "Abc");
	strcpy((char*)t->data + 0x90, "abc");
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x80);
	Op(&a, 0x04);
	A16(&a, 0x70);
	Op(&a, 0x69); // "Abc" == "abc"? no
	Ret(&a);
	CHECK(Run(o, &a) == 4 && Top() == 0);
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x80);
	Op(&a, 0x6d); // lower-case in place
	Op(&a, 0x04);
	A16(&a, 0x80);
	Op(&a, 0x04);
	A16(&a, 0x70);
	Op(&a, 0x69); // now equal
	Ret(&a);
	CHECK(Run(o, &a) == 4 && Top() == 1);
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x60); // dst = data[0xa0]
	Op(&a, 0x04);
	A16(&a, 0x80); // a
	Op(&a, 0x04);
	A16(&a, 0x70); // b
	Op(&a, 0x6b);  // dst = a + b
	Ret(&a);
	CHECK(Run(o, &a) == 4);
	CHECK(strcmp((char*)t->data + 0xa0, "abcabc") == 0);
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x50); // dst = data[0xb0]
	Op(&a, 0x04);
	A16(&a, 0x80);
	PushI8(&a, '"');
	Op(&a, 0x6e); // dst = the string between two quote characters
	Ret(&a);
	CHECK(Run(o, &a) == 4);
	CHECK(strcmp((char*)t->data + 0xb0, "\"abc\"") == 0);

	// 6C: a character, whether it is double-byte, whether it is a kinsoku character
	strcpy((char*)t->data + 0x80, "\x82\xa0x"); // あ (the hiragana "a") followed by an "x"
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x80);
	Op(&a, 0x6c);
	Ret(&a);
	CHECK(Run(o, &a) == 4);
	CHECK(Depth() == 3);
	CHECK(Top() == 0); // あ is not a kinsoku character
	CHECK(t->stack[1] == 1);
	CHECK(t->stack[0] == 0x82a0);
	strcpy((char*)t->data + 0x80, "\x81\x42"); // 。 (the full stop): kinsoku
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x80);
	Op(&a, 0x6c);
	Ret(&a);
	CHECK(Run(o, &a) == 4);
	CHECK(Top() == 1 && t->stack[1] == 1 && t->stack[0] == 0x8142);

	// 6F: the arguments are popped in order of appearance after the format and
	// the destination, so the script pushes them in reverse
	strcpy((char*)t->data + 0x80, "%d-%04x-%s-%%-%c");
	strcpy((char*)t->data + 0xc0, "str");
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	PushI8(&a, 'Z'); // %c
	Op(&a, 0x04);
	A16(&a, 0x40);              // %s = data[0xc0]
	PushI32(&a, 0xbeef);        // %04x
	PushI32(&a, (uint32_t)-12); // %d
	Op(&a, 0x04);
	A16(&a, 0x60); // dst = data[0xa0]
	Op(&a, 0x04);
	A16(&a, 0x80); // fmt
	Op(&a, 0x6f);
	Ret(&a);
	CHECK(Run(o, &a) == 4);
	CHECK(Depth() == 0); // every argument was consumed
	CHECK(strcmp((char*)t->data + 0xa0, "-12-beef-str-%-Z") == 0);

	/* the destination as a source: `strcat(buf, buf, "x")` appends (what the
	 * scenario libraries build every file name with), `strcpy(s, s)` keeps
	 * the string, a sprintf argument may be the destination, and so may the
	 * quoted string - all as the original's CRT did it, whatever this one does */
	strcpy((char*)t->data + 0x80, "bs_a11");
	strcpy((char*)t->data + 0x90, "_N");
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	Op(&a, 0x04);
	A16(&a, 0x80); // dst = data[0x80]
	Op(&a, 0x04);
	A16(&a, 0x80); // a = the same
	Op(&a, 0x04);
	A16(&a, 0x70); // b
	Op(&a, 0x6b);
	Op(&a, 0x04);
	A16(&a, 0x80);
	Op(&a, 0x04);
	A16(&a, 0x80);
	Op(&a, 0x6a); // strcpy(s, s)
	Op(&a, 0x04);
	A16(&a, 0x80);
	Op(&a, 0x04);
	A16(&a, 0x80);
	PushI8(&a, '\'');
	Op(&a, 0x6e); // quote in place
	Ret(&a);
	CHECK(Run(o, &a) == 4);
	CHECK(strcmp((char*)t->data + 0x80, "'bs_a11_N'") == 0);
	strcpy((char*)t->data + 0x80, "%s+%d");
	strcpy((char*)t->data + 0xa0, "one");
	a.len = 0;
	PushI32(&a, 0x100);
	Op(&a, 0x11);
	PushI8(&a, 2); // %d
	Op(&a, 0x04);
	A16(&a, 0x60); // %s = the destination
	Op(&a, 0x04);
	A16(&a, 0x60); // dst = data[0xa0]
	Op(&a, 0x04);
	A16(&a, 0x80); // fmt
	Op(&a, 0x6f);
	Ret(&a);
	CHECK(Run(o, &a) == 4);
	CHECK(strcmp((char*)t->data + 0xa0, "one+2") == 0);
}

/* Control flow: "15" with condition 5 ("< 0") jumps over a push and with
 * condition 2 ("> 0") falls through, the target being a code offset
 * pushed by "06"; "16" pushes the return address on the frame stack in
 * the data area and "17" returns to it, while a return at fp 0 ends the
 * thread.  The targets are patched into the code after the skipped part
 * is assembled. */
static void TestControl(OpenBGI_t* o)
{
	Asm_t a;
	printf("control flow\n");
	// jcc: 5 = "< 0" taken, 2 = "> 0" not taken; the target is a code offset
	a.len = 0;
	PushI32(&a, (uint32_t)-1); // v
	Op(&a, 0x06);
	A16(&a, 0); // placeholder, patched below
	{
		uint32_t at = a.len - 2, opAt = at - 1;
		Op(&a, 0x15);
		A8(&a, 5);
		PushI32(&a, 111); // skipped
		Ret(&a);
		a.code[at] = (uint8_t)(a.len - opAt);
		a.code[at + 1] = (uint8_t)((a.len - opAt) >> 8);
		PushI32(&a, 222);
		Ret(&a);
	}
	CHECK(Run(o, &a) == 4 && Top() == 222 && Depth() == 1);
	a.len = 0;
	PushI32(&a, (uint32_t)-1);
	Op(&a, 0x06);
	A16(&a, 0);
	{
		uint32_t at = a.len - 2, opAt = at - 1;
		Op(&a, 0x15);
		A8(&a, 2); // > 0: not taken
		PushI32(&a, 111);
		Ret(&a);
		a.code[at] = (uint8_t)(a.len - opAt);
		a.code[at + 1] = (uint8_t)((a.len - opAt) >> 8);
		PushI32(&a, 222);
		Ret(&a);
	}
	CHECK(Run(o, &a) == 4 && Top() == 111 && Depth() == 1);

	// call / ret: the return address goes on the frame stack in the data area; ret at fp
	// 0 ends the thread
	a.len = 0;
	PushI32(&a, 0x40);
	Op(&a, 0x11);
	Op(&a, 0x06);
	A16(&a, 0);
	{
		uint32_t at = a.len - 2, opAt = at - 1;
		Op(&a, 0x16);
		PushI32(&a, 5);
		Op(&a, 0x20); // 7 + 5
		Ret(&a);
		a.code[at] = (uint8_t)(a.len - opAt);
		a.code[at + 1] = (uint8_t)((a.len - opAt) >> 8);
		PushI32(&a, 7);
		A8(&a, 0x17); // a plain return: back to the instruction after the call
	}
	CHECK(Run(o, &a) == 4 && Top() == 12 && Depth() == 1);
}

/* The thread-local heap: "70" returns a pointer with the heap tag that
 * ResolvePtr maps to the block, a second block does not overlap the
 * first, and "71" frees each with 1. */
static void TestHeap(OpenBGI_t* o)
{
	Asm_t a;
	Thread_t* t = gT;
	uint32_t p;
	printf("heap\n");
	a.len = 0;
	PushI32(&a, 100);
	Op(&a, 0x70);
	p = Eval(o, &a);
	CHECK((p & 0xfe000000u) == VM_TAG_HEAP(o));
	CHECK(ResolvePtr(o, p, t) == Thread_HeapPtr(t, p & VM_OFFSET_MASK(o)));
	// a second block does not overlap the first; freeing both succeeds, freeing again
	// fails
	a.len = 0;
	PushI32(&a, 100);
	Op(&a, 0x70);
	{
		uint32_t q = Eval(o, &a);
		CHECK(q != p);
		CHECK((q & VM_OFFSET_MASK(o)) >= (p & VM_OFFSET_MASK(o)) + 100 || (p & VM_OFFSET_MASK(o)) >= (q & VM_OFFSET_MASK(o)) + 100);
		a.len = 0;
		PushI32(&a, q);
		Op(&a, 0x71);
		CHECK(Eval(o, &a) == 1);
	}
	a.len = 0;
	PushI32(&a, p);
	Op(&a, 0x71);
	CHECK(Eval(o, &a) == 1);
}

/* Script errors: a jump to address 0 ("14" with a NULL target) raises a
 * script error, which arrives in the try frame as the thrown value
 * BGI_SCRIPT_ERROR_VALUE; an undefined base opcode ("03") has no handler,
 * which Run reports as -1 (the scheduler raises MSG_UNDEFINED_OP
 * instead).  A failure means bad scripts would run on instead of stopping
 * with an error box. */
static void TestErrors(OpenBGI_t* o)
{
	Asm_t a;
	BgiTryFrame_t f;
	printf("errors\n");
	a.len = 0;
	PushI32(&a, 0);
	Op(&a, 0x14); // jump to NULL
	Ret(&a);
	if(BGI_TRY(o, f))
	{
		Run(o, &a);
		BGI_END_TRY(o, f);
		CHECK(!"a jump to NULL must raise");
	}
	else
	{
		CHECK(f.value == BGI_SCRIPT_ERROR_VALUE);
	}
	a.len = 0;
	Op(&a, 0x03); // undefined base instruction
	Ret(&a);
	if(BGI_TRY(o, f))
	{
		int rc = Run(o, &a);
		BGI_END_TRY(o, f);
		CHECK(rc == -1); // Run stops on the NULL handler; the scheduler raises MSG_UNDEFINED_OP
	}
	else
	{
		CHECK(f.value == BGI_SCRIPT_ERROR_VALUE);
	}
}

/* The "EF xx" family (src/vm/ops_obgi.c, docs/modding/obgi.md): "EF 00" pushes
 * the level of the extensions; the switches take their arguments off the
 * stack, push nothing, and land in the text engine's variables - a
 * direction other than 1 or 2 is 0, an encoding other than 0 or 1 changes
 * nothing, a scroll time of 0 keeps the one that is set.  A number the
 * family does not have is an undefined instruction, which the dispatcher
 * raises as a script error. */
static void TestExtensions(OpenBGI_t* o)
{
	Asm_t a;
	BgiTryFrame_t f;
	int mode;
	static const int dirs[][2] = {{1, 1}, {2, 2}, {0, 0}, {5, 0}, {-1, 0}};
	printf("extensions\n");
	a.len = 0;
	Op(&a, 0xef);
	A8(&a, 0x00); // obgi.version
	CHECK(Eval(o, &a) == 2);
	for(mode = 0; mode < 5; mode++)
	{
		a.len = 0;
		PushI8(&a, dirs[mode][0]);
		Op(&a, 0xef);
		A8(&a, 0x01);  // obgi.text_direction
		PushI8(&a, 7); // (what Eval expects to find: the instruction itself pushes nothing)
		CHECK(Eval(o, &a) == 7);
		CHECK(o->text.textDirection == dirs[mode][1]);
	}
	a.len = 0;
	PushI8(&a, 1);
	Op(&a, 0xef);
	A8(&a, 0x02); // obgi.text_encoding(1): UTF-8
	PushI8(&a, 7);
	CHECK(Eval(o, &a) == 7);
	CHECK(o->enc.textUtf8 == 1);
	a.len = 0;
	PushI8(&a, 9); // not an encoding: nothing changes
	Op(&a, 0xef);
	A8(&a, 0x02);
	PushI8(&a, 0);
	Op(&a, 0xef);
	A8(&a, 0x02); // and back to Shift-JIS
	PushI8(&a, 7);
	CHECK(Eval(o, &a) == 7);
	CHECK(o->enc.textUtf8 == 0);
	a.len = 0;
	PushI8(&a, 1);
	Op(&a, 0xef);
	A8(&a, 0x03); // obgi.text_word_wrap(1)
	PushI8(&a, 1);
	PushI16(&a, 200);
	Op(&a, 0xef);
	A8(&a, 0x08); // obgi.text_scroll(1, 200)
	PushI8(&a, 7);
	CHECK(Eval(o, &a) == 7);
	CHECK(o->text.textWordWrap == 1 && o->text.textScroll == 1 && o->text.textScrollMs == 200);
	a.len = 0;
	PushI8(&a, 0);
	Op(&a, 0xef);
	A8(&a, 0x03);
	PushI8(&a, 0);
	PushI8(&a, 0); // a time of 0 keeps the 200
	Op(&a, 0xef);
	A8(&a, 0x08);
	PushI8(&a, 7);
	CHECK(Eval(o, &a) == 7);
	CHECK(o->text.textWordWrap == 0 && o->text.textScroll == 0 && o->text.textScrollMs == 200);
	a.len = 0;
	Op(&a, 0xef);
	A8(&a, 0x7f); // no such instruction in the family
	Ret(&a);
	if(BGI_TRY(o, f))
	{
		Run(o, &a);
		BGI_END_TRY(o, f);
		CHECK(!"an undefined EF instruction must raise");
	}
	else
	{
		CHECK(f.value == BGI_SCRIPT_ERROR_VALUE);
	}
}

/* 1 when CheckPriority accepts prio, 0 when it raises the script error */
static int PriorityAccepted(OpenBGI_t* o, uint32_t prio)
{
	BgiTryFrame_t f;
	if(BGI_TRY(o, f))
	{
		CheckPriority(o, prio, gT);
		BGI_END_TRY(o, f);
		return 1;
	}
	CHECK(f.value == BGI_SCRIPT_ERROR_VALUE);
	return 0;
}

/* ThrowScriptError(text) in a try frame; 1 when it threw the script error */
static int Throws(OpenBGI_t* o, const char* text)
{
	BgiTryFrame_t f;
	if(BGI_TRY(o, f))
	{
		ThrowScriptError(o, text);
		BGI_END_TRY(o, f);
		return 0;
	}
	return f.value == BGI_SCRIPT_ERROR_VALUE;
}

/* The services the instructions of the later builds rely on.  A display
 * priority is below 0x1000, from 1.529 on below 0x10000.  While "81 6A"
 * keeps errors the first one's message is kept and a later error does
 * not replace it ("81 6B" reads it: its length + 1, 0 when none).  The
 * gamepad table of "81 1B" has 36 entries; "81 04" takes a step limit of
 * 50 .. 60000 ms and "81 68" answers the previous setting. */
static void TestLaterBuilds(OpenBGI_t* o)
{
	char buf[32];
	printf("later builds\n");
	CHECK(PriorityAccepted(o, 0xfff));
	CHECK(!PriorityAccepted(o, 0x1000));
	CHECK(Engine_SelectProfile(o, "1.636"));
	CHECK(PriorityAccepted(o, 0xffff));
	CHECK(!PriorityAccepted(o, 0x10000));
	CHECK(Error_KeptMessage(o, NULL) == 0);
	Error_SetKeep(o, 1);
	CHECK(Throws(o, "first"));
	CHECK(Throws(o, "the second one"));
	CHECK(Error_KeptMessage(o, NULL) == 6);
	memset(buf, 0x55, sizeof buf);
	CHECK(Error_KeptMessage(o, buf) == 6 && strcmp(buf, "first") == 0);
	Error_FreeKept(o);
	Error_SetKeep(o, 0);
	CHECK(Error_KeptMessage(o, NULL) == 0);
	CHECK(Pad_SetKey(o, 35, 0x0d) == 0 && o->input.padKeys[35] == 0x0d);
	CHECK(Pad_SetKey(o, 36, 0x0d) == 0x80000002u);
	CHECK(Clock_SetMaxStep(o, 49) == 0 && Clock_SetMaxStep(o, 60001) == 0);
	CHECK(Clock_SetMaxStep(o, 60000) == 1 && Clock_SetMaxStep(o, 500) == 1);
	CHECK(Clock_SetStopWhenInactive(o, 1) == 0 && Clock_SetStopWhenInactive(o, 0) == 1);
	CHECK(Engine_SelectProfile(o, "1.69/444"));
}

// step thread t from where it is until a handler returns a non-zero scheduler code; -1 for an opcode without a handler
static int Step(OpenBGI_t* o, Thread_t* t)
{
	int rc = 0, steps = 0;
	while(rc == 0 && steps++ < 100000)
	{
		uint8_t op = Thread_FetchOp(t);
		if(!o->vm.optablemain[op])
			return -1;
		rc = Settle(o, t, o->vm.optablemain[op](o, t));
	}
	return rc;
}

// load the program into t's code area at offset 0 and step it from there with an empty stack
static int RunOn(OpenBGI_t* o, Thread_t* t, const Asm_t* a)
{
	memcpy(t->code, a->code, a->len);
	t->codeUsed = a->len;
	Thread_SetIP(t, 0);
	t->sp = 0;
	return Step(o, t);
}

/* "81 44": threads in parts of another thread's areas.  The parts are cut
 * from the tops of the lender's code and data areas, and what the lender
 * may use itself ends below them; a second borrower goes below the first,
 * and a borrower's own borrower borrows from the first lender.  A code
 * part that would reach into the lender's modules fails with 2, a data
 * part larger than the room left with 3.  A borrower shares the lender's
 * buffers and heap, loads its modules into its part (and may unload them
 * all with "80 41", which is an error for a thread's main program), keeps
 * its frames in its part of the data area and ends with a return at the
 * start of that part.  A borrower that ends gives its parts back (the lender's limits
 * stay until its next loan); a lender that ends takes its borrowers with
 * it.  Through the instruction: the id is pushed, an ip past the part is
 * a script error. */
static void TestBorrower(OpenBGI_t* o)
{
	Thread_t* root = Thread_New(o, 0, 0, 0);
	Thread_t *l, *a, *b, *g = NULL;
	uint8_t prog[16 + 0x40];
	ProgramHeader_t* h = (ProgramHeader_t*)prog;
	Asm_t s;
	BgiTryFrame_t f;
	printf("borrowed areas\n");
	CHECK(Engine_SelectProfile(o, "1.636"));
	Vm_TablesInit(o);
	l = Thread_SpawnChild(o, root, 0x100, 0x1000, 0x1000);
	memset(prog, 0, sizeof prog);
	h->codeOffset = 16;
	h->codeSize = 0x40;
	CHECK(Thread_LoadModule(o, l, prog, "lender") == 0 && l->codeUsed == 0x40);
	CHECK(Thread_SpawnBorrower(o, l, 0x40, 0x400, 0x300, 0x10, &a) == 0);
	CHECK(a->lender == l && a->code == l->code && a->data == l->data && a->heap == l->heap);
	CHECK(a->codeBase == 0xc00 && a->codeUsed == 0xc00 && a->codeSize == 0x1000);
	CHECK(a->dataBase == 0xd00 && a->fp == 0xd00 && a->dataSize == 0x1000 && a->ip == 0x10);
	CHECK(l->codeSize == 0xc00 && l->dataSize == 0xd00 && l->loanCount == 1 && l->next == a);
	CHECK(a->id == l->id + 1 && a->stackEntries == 0x40);
	CHECK(Thread_SpawnBorrower(o, a, 0x40, 0x200, 0x100, 0x20, &b) == 0); // from a's lender
	CHECK(b->lender == l && b->codeBase == 0xa00 && b->dataBase == 0xc00 && a->next == b);
	CHECK(l->codeSize == 0xa00 && l->dataSize == 0xc00 && l->loanCount == 2);
	CHECK(Thread_SpawnBorrower(o, l, 0x40, 0x9c1, 0x10, 0, &g) == 2 && g == NULL);                      // into the lender's module
	CHECK(Thread_SpawnBorrower(o, l, 0x40, 0x10, 0xc01, 0, &g) == 3 && g == NULL && l->loanCount == 2); // more data than is left
	CHECK(Thread_LoadModule(o, a, prog, "borrower") == 0xc00 && a->codeUsed == 0xc40);
	CHECK(Thread_LoadModule(o, l, prog, "lender2") == 0x40); // the lender's own loads stay below its loans

	// a call and the returns of a borrower: its frames are in the lender's data area
	s.len = 0;
	PushI32(&s, 0xa0);
	Op(&s, 0x16); // call 0xa0
	PushI32(&s, 9);
	Op(&s, 0x17); // the outermost return: the thread ends
	while(s.len < 0x20)
		Op(&s, 0xff);
	PushI32(&s, 7);
	Op(&s, 0x17);
	memcpy(l->code + 0x80, s.code, s.len);
	Thread_SetIP(b, 0x80);
	b->sp = 0;
	CHECK(Step(o, b) == 4 && b->sp == 2 && b->stack[0] == 7 && b->stack[1] == 9 && b->fp == 0xc00);
	CHECK(l->data[0xc00] == 0x86); // the return address, at the start of b's part

	// a borrower that ends gives its parts back, the lender's limits stay; a lender takes its borrowers along
	CHECK(Thread_SpawnBorrower(o, l, 0x40, 0x100, 0x100, 0, &g) == 0 && g->codeBase == 0x900 && l->loanCount == 3);
	CHECK(Thread_KillChild(o, root, g) && l->loanCount == 2 && l->codeSize == 0x900);
	CHECK(Thread_SpawnBorrower(o, l, 0x40, 0x80, 0x80, 0, &g) == 0 && g->codeBase == 0x980 && g->dataBase == 0xb80);
	CHECK(l->codeSize == 0x980 && l->dataSize == 0xb80);
	CHECK(Thread_KillChild(o, root, l) && root->next == NULL);
	Thread_Delete(o, root);

	// the instruction: stackEntries, codeSize, dataSize, ip -> id; an ip past the new part is a script error
	l = Thread_New(o, 0x100, 0x1000, 0x1000);
	s.len = 0;
	PushI32(&s, 0x40);
	PushI32(&s, 0x200);
	PushI32(&s, 0x200);
	PushI32(&s, 0x10);
	A8(&s, 0x81);
	A8(&s, 0x44);
	Op(&s, 0x17);
	CHECK(RunOn(o, l, &s) == 4 && l->sp == 1 && l->next && l->stack[0] == l->next->id);
	CHECK(l->next && l->next->codeBase == 0xe00 && l->next->ip == 0x10);
	// "80 41": a borrower may unload its last module (its main program is the lender's), the lender may not
	g = l->next;
	CHECK(Thread_LoadModule(o, g, prog, "sub") == 0xe00);
	s.len = 0;
	A8(&s, 0x80);
	A8(&s, 0x41);
	A8(&s, 0x80);
	A8(&s, 0x41);
	Op(&s, 0x17);
	memcpy(l->code + 0x100, s.code, s.len);
	Thread_SetIP(g, 0x100);
	g->sp = 0;
	CHECK(Step(o, g) == 4 && g->sp == 2 && g->stack[0] == 0 && g->stack[1] == 0x80000001u && g->codeUsed == 0xe00);
	CHECK(Thread_LoadModule(o, l, prog, "main") != 0x80000000u);
	if(BGI_TRY(o, f))
	{
		RunOn(o, l, &s);
		BGI_END_TRY(o, f);
		CHECK(!"no script error for unloading the main program");
	}
	else
		CHECK(f.value == BGI_SCRIPT_ERROR_VALUE);
	s.len = 0;
	PushI32(&s, 0x40);
	PushI32(&s, 0x200);
	PushI32(&s, 0x200);
	PushI32(&s, 0xe00); // the end of the new part, 0xc00 .. 0xe00
	A8(&s, 0x81);
	A8(&s, 0x44);
	Op(&s, 0x17);
	if(BGI_TRY(o, f))
	{
		RunOn(o, l, &s);
		BGI_END_TRY(o, f);
		CHECK(!"no script error for an ip past the part");
	}
	else
		CHECK(f.value == BGI_SCRIPT_ERROR_VALUE && l->next && l->next->next && l->next->next->codeBase == 0xc00);
	while(l->next)
		Thread_KillChild(o, l, l->next);
	CHECK(l->loanCount == 0 && !l->codeLoans && !l->dataLoans);
	Thread_Delete(o, l);
	CHECK(Engine_SelectProfile(o, "1.69/444"));
	Vm_TablesInit(o);
}

// a pointer to byte `off` of the data area, with the selected profile's tag
static uint32_t DataPtr(OpenBGI_t* o, uint32_t off)
{
	return off | (o->engine.profile->tagData << VM_TAG_SHIFT(o));
}

/* push the n arguments, run family `fam` instruction `op` and return what it pushed */
static uint32_t Call(OpenBGI_t* o, uint32_t fam, uint32_t op, const uint32_t* args, int n)
{
	Asm_t a;
	int i;
	a.len = 0;
	for(i = 0; i < n; i++)
		PushI32(&a, args[i]);
	A8(&a, fam);
	A8(&a, op);
	return Eval(o, &a);
}

// the contents of a file as a string (at most 63 bytes); "" when it cannot be read
static const char* Slurp(const char* path)
{
	static char buf[64];
	FILE* f = fopen(path, "rb");
	size_t n = 0;
	if(f)
	{
		n = fread(buf, 1, sizeof buf - 1, f);
		fclose(f);
	}
	buf[n] = 0;
	return buf;
}

// write a string to a file
static void Spit(const char* path, const char* text)
{
	FILE* f = fopen(path, "wb");
	if(f)
	{
		fputs(text, f);
		fclose(f);
	}
}

/* Files, through the instructions, with two files under bin/ (the test
 * runs from the project root).  "80 27" moves the operand pushed last to
 * the one pushed first and, as MoveFile, fails when the destination
 * exists.  "81 28" opens (0 read, 1 write from scratch, 2 write into the
 * file as it is) and refuses a bad mode (1), a path already open in
 * another case (2) and a missing file (3); the status words of "81 29" ..
 * "81 2B" get the bytes transferred, 1 for a seek or a close, -1 for
 * nothing transferred or a seek past the end, and an unknown handle is
 * 4.  In 1.553 there is no mode 2, "81 29" takes only the handle (-1 when
 * unknown), a transfer of 0 bytes is 6, one that transfers nothing gives
 * -2, and there is no "81 2B".  "81 38" answers 7 without filters, 4 for
 * an unknown kind (the path zeroed) and -1 when nothing was chosen. */
static void TestFiles(OpenBGI_t* o)
{
	static const char* A = "bin/test_vm_a.tmp";
	static const char* B = "bin/test_vm_b.tmp";
	uint8_t* d = gT->data;
	uint32_t pa, pb, pbUpper, out, st, buf, h, h2;
	int32_t status;
	printf("files\n");
	CHECK(Engine_SelectProfile(o, "1.636"));
	Vm_TablesInit(o);
	pa = DataPtr(o, 0x400), pb = DataPtr(o, 0x480), pbUpper = DataPtr(o, 0x500);
	out = DataPtr(o, 0x200), st = DataPtr(o, 0x210), buf = DataPtr(o, 0x300);
	remove(A);
	remove(B);
	strcpy((char*)d + 0x400, A);
	strcpy((char*)d + 0x480, B);
	strcpy((char*)d + 0x500, "BIN/TEST_VM_B.TMP");
	Spit(A, "hello");
	{
		const uint32_t mv[] = {pb, pa}; // newPath, existing
		CHECK(Call(o, 0x80, 0x27, mv, 2) == 1 && strcmp(Slurp(B), "hello") == 0 && strcmp(Slurp(A), "") == 0);
		Spit(A, "again");
		CHECK(Call(o, 0x80, 0x27, mv, 2) == 0 && strcmp(Slurp(B), "hello") == 0); // the destination exists
		remove(A);
	}
	{
		const uint32_t badMode[] = {out, pb, 3}, missing[] = {out, pa, 0}, rd[] = {out, pb, 0}, again[] = {out, pbUpper, 0};
		CHECK(Call(o, 0x81, 0x28, badMode, 3) == 1);
		CHECK(Call(o, 0x81, 0x28, missing, 3) == 3);
		CHECK(Call(o, 0x81, 0x28, rd, 3) == 0);
		memcpy(&h, d + 0x200, 4);
		CHECK(h != 0);
		CHECK(Call(o, 0x81, 0x28, again, 3) == 2); // the same file in another case
	}
	{
		const uint32_t rd4[] = {st, h, buf, 4}, rd9[] = {st, h, buf + 4, 9}, rd1[] = {st, h, buf, 1};
		const uint32_t end[] = {st, h, 0xffffffffu}, past[] = {st, h, 6}, start[] = {st, h, 1}, close[] = {st, h};
		const uint32_t unknown[] = {st, h + 100, buf, 4};
		memset(d + 0x300, 0, 16);
		CHECK(Call(o, 0x81, 0x2a, rd4, 4) == 0 && (memcpy(&status, d + 0x210, 4), status == 4) && memcmp(d + 0x300, "hell", 4) == 0);
		CHECK(Call(o, 0x81, 0x2a, rd9, 4) == 0 && (memcpy(&status, d + 0x210, 4), status == 1) && d[0x304] == 'o');
		CHECK(Call(o, 0x81, 0x2a, rd1, 4) == 0 && (memcpy(&status, d + 0x210, 4), status == -1)); // at the end
		CHECK(Call(o, 0x81, 0x2a, unknown, 4) == 4);
		CHECK(Call(o, 0x81, 0x2b, past, 3) == 0 && (memcpy(&status, d + 0x210, 4), status == -1));
		CHECK(Call(o, 0x81, 0x2b, end, 3) == 0 && (memcpy(&status, d + 0x210, 4), status == 1));
		CHECK(Call(o, 0x81, 0x2b, start, 3) == 0 && (memcpy(&status, d + 0x210, 4), status == 1));
		CHECK(Call(o, 0x81, 0x2a, rd4, 4) == 0 && memcmp(d + 0x300, "ello", 4) == 0);
		CHECK(Call(o, 0x81, 0x29, close, 2) == 0 && (memcpy(&status, d + 0x210, 4), status == 1));
		CHECK(Call(o, 0x81, 0x29, close, 2) == 4);
	}
	{
		const uint32_t keep[] = {out, pb, 2}, create[] = {out, pb, 1};
		uint32_t w2[4], end[3], w1[4], close[2];
		memcpy(d + 0x300, "XYZ", 3);
		CHECK(Call(o, 0x81, 0x28, keep, 3) == 0);
		memcpy(&h2, d + 0x200, 4);
		CHECK(h2 > h); // a new number
		w2[0] = st, w2[1] = h2, w2[2] = buf, w2[3] = 2;
		end[0] = st, end[1] = h2, end[2] = 0xffffffffu;
		w1[0] = st, w1[1] = h2, w1[2] = buf + 2, w1[3] = 1;
		close[0] = 0, close[1] = h2; // no status word
		CHECK(Call(o, 0x81, 0x2a, w2, 4) == 0 && (memcpy(&status, d + 0x210, 4), status == 2));
		CHECK(Call(o, 0x81, 0x2b, end, 3) == 0 && Call(o, 0x81, 0x2a, w1, 4) == 0);
		CHECK(Call(o, 0x81, 0x29, close, 2) == 0 && strcmp(Slurp(B), "XYlloZ") == 0);
		CHECK(Call(o, 0x81, 0x28, create, 3) == 0);
		memcpy(&h2, d + 0x200, 4);
		close[1] = h2;
		CHECK(Call(o, 0x81, 0x29, close, 2) == 0 && strcmp(Slurp(B), "") == 0); // truncated
	}
	{
		uint32_t list = DataPtr(o, 0x700), str = DataPtr(o, 0x780), path = DataPtr(o, 0x800);
		const uint32_t none[] = {path, 0, list, list, 0, 0, 0}, kind[] = {path, 1, list, list, 0, 0, 2}, open[] = {path, 1, list, list, 0, 0, 0};
		memcpy(d + 0x700, &str, 4); // a one-entry list: the string at 0x780
		strcpy((char*)d + 0x780, "*.txt");
		memset(d + 0x800, 0x55, 0x30c);
		CHECK(Call(o, 0x81, 0x38, none, 7) == 7 && d[0x800] == 0x55); // the path untouched
		CHECK(Call(o, 0x81, 0x38, kind, 7) == 4 && d[0x800] == 0 && d[0x800 + 0x30b] == 0);
		CHECK(Call(o, 0x81, 0x38, open, 7) == 0xffffffffu); // nothing chosen (no dialog here)
	}

	// 1.553: two modes, a close without a status word, no seek
	CHECK(Engine_SelectProfile(o, "1.553"));
	Vm_TablesInit(o);
	pb = DataPtr(o, 0x480), out = DataPtr(o, 0x200), st = DataPtr(o, 0x210), buf = DataPtr(o, 0x300);
	CHECK(!o->vm.optable81[0x2b]);
	Spit(B, "abc");
	{
		const uint32_t keep[] = {out, pb, 2}, rd[] = {out, pb, 0};
		uint32_t zero[4], all[4], more[4], close[1];
		CHECK(Call(o, 0x81, 0x28, keep, 3) == 1);
		CHECK(Call(o, 0x81, 0x28, rd, 3) == 0);
		memcpy(&h, d + 0x200, 4);
		zero[0] = st, zero[1] = h, zero[2] = buf, zero[3] = 0;
		all[0] = st, all[1] = h, all[2] = buf, all[3] = 8;
		more[0] = st, more[1] = h, more[2] = buf, more[3] = 8;
		close[0] = h;
		CHECK(Call(o, 0x81, 0x2a, zero, 4) == 6);
		CHECK(Call(o, 0x81, 0x2a, all, 4) == 0 && (memcpy(&status, d + 0x210, 4), status == 3));
		CHECK(Call(o, 0x81, 0x2a, more, 4) == 0 && (memcpy(&status, d + 0x210, 4), status == -2));
		CHECK(Call(o, 0x81, 0x29, close, 1) == 0);
		CHECK(Call(o, 0x81, 0x29, close, 1) == 0xffffffffu);
	}
	FileAcc_CloseAll(o);
	remove(A);
	remove(B);
	CHECK(Engine_SelectProfile(o, "1.69/444"));
	Vm_TablesInit(o);
}

// make the thread, run every group; exit status 1 when any check failed
// what the host does with the job that waits: taken, run with the engine `w`, handed back; 1 when there was one
static int HostReads(OpenBGI_t* o, OpenBGI_t* w)
{
	OpenBGI_Job_t* job = OpenBGI_JobNext(o);
	if(!job)
		return 0;
	OpenBGI_JobRun(job, w);
	OpenBGI_JobDone(o, job);
	return 1;
}

static void TestLauncherWaits(OpenBGI_t* o)
{
	static const char* DIR = "bin/test_vm_dir";
	uint8_t* d = gT->data;
	uint32_t path = DataPtr(o, 0x100), info = DataPtr(o, 0x600), h;
	OpenBGI_t* w;
	OpenBGI_Job_t* job;
	Asm_t list, look, close;
	if(!(o->host.caps & OPENBGI_CAP_JOBS))
		return; // (a host that does not: nothing waits)
	w = OpenBGI_New(OS_Host());
	CHECK(w != NULL);
	if(!w)
		return;
	strcpy((char*)d + 0x100, DIR);
	list.len = 0;
	PushI32(&list, path);
	PushI8(&list, 6); // the flags: for games, and deeper
	A8(&list, 0xef);
	A8(&list, 0x20);
	Ret(&list);
	look.len = 0;
	PushI32(&look, path);
	PushI32(&look, info);
	A8(&look, 0xef);
	A8(&look, 0x28);
	Ret(&look);
	o->launcher.running = 1;

	// the listing: the thread waits, the job goes out and comes back, the thread has its handle
	CHECK(Run(o, &list) == 2 && (Thread_GetFlags(gT) & THR_FLAG_WAITING) && Depth() == 0);
	CHECK(Thread_PollWait(o, gT) == 0);
	job = OpenBGI_JobNext(o);
	CHECK(job != NULL && OpenBGI_JobNext(o) == NULL && Thread_PollWait(o, gT) == 0);
	OpenBGI_JobRun(job, w);
	CHECK(Thread_PollWait(o, gT) == 0); // (run, and not handed back yet)
	OpenBGI_JobDone(o, job);
	CHECK(Thread_PollWait(o, gT) == 1 && Depth() == 1 && !(Thread_GetFlags(gT) & THR_FLAG_WAITING));
	h = Top();
	CHECK(Step(o, gT) == 4 && h >= 1 && Call(o, 0xef, 0x21, &h, 1) == 3);
	close.len = 0;
	PushI32(&close, h);
	A8(&close, 0xef);
	A8(&close, 0x23);
	PushI8(&close, 7);
	CHECK(Eval(o, &close) == 7);
	// what it announced was queued: the look at the archive in it and the listing of its folder.  Read ahead,
	// the look is answered at once
	CHECK(HostReads(o, w) && HostReads(o, w) && !HostReads(o, w));
	snprintf((char*)d + 0x100, 0x100, "%s%cA.zip", DIR, Host_NativeSep(o)); // (the name joined to the folder as the system does it)
	memset(d + 0x600, 0xff, 212);
	CHECK(Run(o, &look) == 4 && Depth() == 1 && Top() == 0 && d[0x600] == 0 && d[0x600 + 211] == 0);
	// a look that was not: the thread waits for it
	strcpy((char*)d + 0x100, DIR);
	memset(d + 0x600, 0xff, 212);
	CHECK(Run(o, &look) == 2 && d[0x600] == 0xff);
	CHECK(HostReads(o, w) && Thread_PollWait(o, gT) == 1 && Top() == 0 && d[0x600] == 0 && d[0x600 + 211] == 0);
	CHECK(Step(o, gT) == 4);
	// a thread that stops waiting while its job is out: the job can still be handed back
	CHECK(Run(o, &list) == 2);
	job = OpenBGI_JobNext(o);
	CHECK(job != NULL);
	Thread_DropWait(o, gT);
	if(job)
	{
		OpenBGI_JobRun(job, w);
		OpenBGI_JobDone(o, job);
	}
	CHECK(OpenBGI_JobNext(o) == NULL && o->launcher.jobsOut == 0);
	// the run ends while a thread waits: it goes on with nothing
	CHECK(Run(o, &look) == 2);
	Launcher_RunEnded(o);
	CHECK(!o->launcher.running && Thread_PollWait(o, gT) == 1 && Top() == 0 && d[0x600] == 0);
	CHECK(Step(o, gT) == 4);
	// .. and outside a launcher's run nothing waits
	CHECK(Run(o, &look) == 4 && Top() == 0);
	OpenBGI_Delete(w);
}

/* The launcher's instructions, with a folder made under bin/.  "EF 20"
 * reads a folder: its folders first, then its files, each by name without
 * regard to case, the hidden ones only with flag 1 and then last; "EF 22"
 * gives an entry's name and kind (2 a folder, 1 a file, 0 none).  "EF 2C"
 * and "EF 2D" read and write the settings: -1 for a value that is not
 * there, a value 0 deletes; "separator" and "language" are the engine's
 * word about the host and cannot be set.  "EF 28" finds no game in a
 * folder without one, and "EF 29" then starts nothing.  In a launcher's
 * run on a host that reads beside the frames (OPENBGI_CAP_JOBS), "EF 20"
 * and "EF 28" wait: the thread goes on with the same results once the
 * host has run the job and handed it back, at once when the answer was
 * read ahead, and with 0 when the run ends first; a thread that stops
 * waiting leaves a job the host can still hand back. */
static void TestLauncher(OpenBGI_t* o)
{
	static const char* DIR = "bin/test_vm_dir";
	uint8_t* d = gT->data;
	uint32_t path = DataPtr(o, 0x100), key = DataPtr(o, 0x300), out = DataPtr(o, 0x400), val = DataPtr(o, 0x500), info = DataPtr(o, 0x600);
	uint32_t h, args[5];
	char file[64];
	printf("launcher\n");
	strcpy((char*)d + 0x100, DIR);
	Call(o, 0x80, 0x28, &path, 1); // mkdir
	strcpy((char*)d + 0x100, "bin/test_vm_dir/Sub");
	Call(o, 0x80, 0x28, &path, 1);
	Spit("bin/test_vm_dir/b.txt", "b");
	Spit("bin/test_vm_dir/A.zip", "a");
	Spit("bin/test_vm_dir/.hidden", "h");
	strcpy((char*)d + 0x100, DIR);

	args[0] = path;
	args[1] = 0;
	h = Call(o, 0xef, 0x20, args, 2);
	CHECK(h >= 1 && Call(o, 0xef, 0x21, &h, 1) == 3);
	args[0] = h;
	args[2] = out;
	args[3] = 64;
	args[1] = 0;
	CHECK(Call(o, 0xef, 0x22, args, 4) == 2 && strcmp((char*)d + 0x400, "Sub") == 0);
	args[1] = 1;
	CHECK(Call(o, 0xef, 0x22, args, 4) == 1 && strcmp((char*)d + 0x400, "A.zip") == 0);
	args[1] = 2;
	CHECK(Call(o, 0xef, 0x22, args, 4) == 1 && strcmp((char*)d + 0x400, "b.txt") == 0);
	args[1] = 3;
	CHECK(Call(o, 0xef, 0x22, args, 4) == 0 && d[0x400] == 0);
	args[1] = 1;
	args[3] = 3; // room for two characters
	CHECK(Call(o, 0xef, 0x22, args, 4) == 1 && strcmp((char*)d + 0x400, "A.") == 0);
	{
		Asm_t a;
		a.len = 0;
		PushI32(&a, h);
		A8(&a, 0xef);
		A8(&a, 0x23); // dir_close pushes nothing
		PushI8(&a, 7);
		CHECK(Eval(o, &a) == 7);
	}
	CHECK(Call(o, 0xef, 0x21, &h, 1) == 0); // closed
	args[0] = path;
	args[1] = 1;
	h = Call(o, 0xef, 0x20, args, 2);
	CHECK(h >= 1 && Call(o, 0xef, 0x21, &h, 1) == 4);
	args[0] = h;
	args[1] = 3;
	args[2] = out;
	args[3] = 64;
	CHECK(Call(o, 0xef, 0x22, args, 4) == 1 && strcmp((char*)d + 0x400, ".hidden") == 0);
	{
		Asm_t a;
		a.len = 0;
		PushI32(&a, h);
		A8(&a, 0xef);
		A8(&a, 0x23);
		PushI8(&a, 7);
		CHECK(Eval(o, &a) == 7);
	}
	strcpy((char*)d + 0x100, "bin/test_vm_dir/none");
	args[0] = path;
	args[1] = 0;
	CHECK(Call(o, 0xef, 0x20, args, 2) == 0);

	// no game here: nothing to find, nothing to start
	strcpy((char*)d + 0x100, DIR);
	memset(d + 0x600, 0xff, 212);
	args[0] = path;
	args[1] = info;
	CHECK(Call(o, 0xef, 0x28, args, 2) == 0 && d[0x600] == 0 && d[0x600 + 211] == 0);
	d[0x300] = 0;
	args[0] = path;
	args[1] = key; // the profile: ""
	args[2] = 0;   // no patches
	args[3] = key; // no options
	CHECK(Call(o, 0xef, 0x29, args, 4) == 0);
	TestLauncherWaits(o);

	// the settings: path "" is the launcher's own place
	d[0x100] = 0;
	strcpy((char*)d + 0x300, "look");
	strcpy((char*)d + 0x500, "waves");
	args[0] = path;
	args[1] = key;
	args[2] = 0;
	args[3] = out;
	args[4] = 64;
	CHECK(Call(o, 0xef, 0x2c, args, 5) == 0xffffffffu && d[0x400] == 0);
	args[3] = val;
	CHECK(Call(o, 0xef, 0x2d, args, 4) == 1);
	args[3] = out;
	CHECK(Call(o, 0xef, 0x2c, args, 5) == 5 && strcmp((char*)d + 0x400, "waves") == 0);
	args[2] = 1;
	CHECK(Call(o, 0xef, 0x2c, args, 5) == 0xffffffffu);
	args[2] = 0;
	args[3] = 0; // a value 0 deletes
	CHECK(Call(o, 0xef, 0x2d, args, 4) == 1);
	args[3] = out;
	CHECK(Call(o, 0xef, 0x2c, args, 5) == 0xffffffffu);
	// what the engine says of the host
	strcpy((char*)d + 0x300, "separator");
	CHECK(Call(o, 0xef, 0x2c, args, 5) == 1 && (d[0x400] == '/' || d[0x400] == '\\') && d[0x401] == 0);
	args[3] = val;
	CHECK(Call(o, 0xef, 0x2d, args, 4) == 0); // not a setting
	args[3] = out;
	strcpy((char*)d + 0x300, "language");
	CHECK(Call(o, 0xef, 0x2c, args, 5) == 2 && (strcmp((char*)d + 0x400, "en") == 0 || strcmp((char*)d + 0x400, "ja") == 0));
	strcpy((char*)d + 0x300, "home");
	CHECK(Call(o, 0xef, 0x2c, args, 5) != 0xffffffffu);
	args[2] = 1;
	CHECK(Call(o, 0xef, 0x2c, args, 5) == 0xffffffffu); // one value

	snprintf(file, sizeof file, "%s/b.txt", DIR);
	remove(file);
	snprintf(file, sizeof file, "%s/A.zip", DIR);
	remove(file);
	snprintf(file, sizeof file, "%s/.hidden", DIR);
	remove(file);
	strcpy((char*)d + 0x100, "bin/test_vm_dir/Sub");
	Call(o, 0x80, 0x29, &path, 1); // rmdir
	strcpy((char*)d + 0x100, DIR);
	Call(o, 0x80, 0x29, &path, 1);
}

// a load ("08") of size code `sz` through the pointer `ptr`
static void Load(Asm_t* a, uint32_t ptr, int sz)
{
	PushI32(a, ptr);
	Op(a, 0x08);
	A8(a, (uint32_t)sz);
}

// the value a program of `a` and a comparison with `k` ("30" or "31") leaves
static uint32_t Compared(OpenBGI_t* o, Asm_t* a, uint32_t k, uint32_t op)
{
	PushI32(a, k);
	Op(a, op);
	return Eval(o, a);
}

/* The engine's own program file.  "81 3F" (1.492) names it, and from then
 * on it is there: it exists, has PROGRAM_IMAGE_SIZE bytes and loads as
 * Program_Image.  A number a program computes from its bytes - here as
 * the game does that has the instruction, 53 times the sum so far plus
 * the next byte - is the number it is, but "30" finds it equal to any
 * constant and "31" not different, 0 and -1 aside; the same when it comes
 * back out of a 32-bit variable, and for the file's size.  Two such
 * numbers compare as they are, and so does one with an ordered
 * comparison.  A variable or a buffer an engine service wrote over is no
 * longer marked; a copy ("60") is; "63" finds the file's bytes equal to
 * others; a freed block leaves no run behind.  A variable of the global
 * memory that holds such a number is noted beside the game
 * (".openbgi/follow.txt") when the memory is written out, and in a run
 * that begins with that note a load of the variable is marked while it
 * reads the number noted - what a save brings back.  "47": a * b / c into
 * a variable, -1 for a zero divisor; defined in 1.492 and not in 1.494,
 * as "81 3F", while "92 F0" is the other way round. */
static void TestProgramFile(OpenBGI_t* o)
{
	uint8_t* d = gT->data;
	const uint8_t* image;
	static const char* GAME = "bin/test_vm_game";
	uint32_t path, buf, buf2, var, zeros, h, v, blk, i;
	uint32_t args[5];
	char name[0x104], game[0x300];
	char* note;
	Asm_t a;
	printf("program file\n");
	TestHost_Mkdir(GAME);
	Host_NativeAbsolute(o, GAME, game, sizeof game); // (a game's folder is known by its full path)
	CHECK(OS_SetGameDir(game));
	Drives_Scan(o);                          // .. and its drive, where there are drives
	CHECK(Fs_OwnWrite(o, "follow.txt", "")); // no note of an earlier run of this test
	CHECK(OpSet_Has(OPFAM_81, 0x3f, GEN_1_492) && !OpSet_Has(OPFAM_81, 0x3f, GEN_1_494));
	CHECK(OpSet_Has(OPFAM_MAIN, 0x47, GEN_1_492) && !OpSet_Has(OPFAM_MAIN, 0x47, GEN_1_494));
	CHECK(!OpSet_Has(OPFAM_92, 0xf0, GEN_1_492) && OpSet_Has(OPFAM_92, 0xf0, GEN_1_494));
	CHECK(Engine_SelectProfile(o, "1.492"));
	Vm_TablesInit(o);
	CHECK(SetGlobalMemSize(o, 0));
	memset(d, 0, 0x1000);
	path = DataPtr(o, 0x40), var = DataPtr(o, 0x180), zeros = DataPtr(o, 0x1a0);
	buf2 = DataPtr(o, 0x200), buf = DataPtr(o, 0x800);

	// "47"
	a.len = 0;
	PushI32(&a, var), PushI32(&a, 7), PushI32(&a, 6), PushI32(&a, 4);
	Op(&a, 0x47);
	Load(&a, var, 2);
	CHECK(Eval(o, &a) == 10);
	a.len = 0;
	PushI32(&a, var), PushI32(&a, (uint32_t)-7), PushI32(&a, 6), PushI32(&a, 0);
	Op(&a, 0x47);
	Load(&a, var, 2);
	CHECK(Eval(o, &a) == 0xffffffffu);

	// no such file until a program asks for it
	CHECK(o->files.programPath[0] == 0 && o->vm.follow.live == 0);
	a.len = 0;
	PushI32(&a, path);
	A8(&a, 0x81), A8(&a, 0x3f);
	PushI32(&a, 1);
	CHECK(Eval(o, &a) == 1);
	snprintf(name, sizeof name, "%s", (const char*)d + 0x40);
	CHECK(strlen(name) > 7 && strcmp(name + strlen(name) - 7, "BGI.exe") == 0);
	image = Program_Image(o);
	CHECK(Fs_FileAttrs(o, name) == OPENBGI_ATTR_ARCHIVE); // it exists
	args[0] = 0, args[1] = path;
	CHECK(Call(o, 0x80, 0x35, args, 2) == PROGRAM_IMAGE_SIZE); // its size
	args[0] = buf, args[1] = 0, args[2] = path;
	CHECK(Call(o, 0x80, 0x30, args, 3) == PROGRAM_IMAGE_SIZE); // loaded
	CHECK(memcmp(d + 0x800, image, PROGRAM_IMAGE_SIZE) == 0);
	CHECK(o->vm.follow.live == 1);

	// three steps of the game's sum over every fifth byte, kept in a variable
	h = 0;
	for(i = 0; i < 15; i += 5)
		h = h * 53 + (uint32_t)(int32_t)(int8_t)image[i];
	a.len = 0;
	Load(&a, buf, 0);
	PushI32(&a, 53), Op(&a, 0x22);
	Load(&a, buf + 5, 0), Op(&a, 0x20);
	PushI32(&a, 53), Op(&a, 0x22);
	Load(&a, buf + 10, 0), Op(&a, 0x20);
	PushI32(&a, var);
	Op(&a, 0x0a), A8(&a, 2);
	Load(&a, var, 2);
	CHECK(Eval(o, &a) == h); // the number it is
	CHECK(h != 0 && h != 0xffffffffu && h != 0x12345678u);
	a.len = 0, Load(&a, var, 2);
	CHECK(Compared(o, &a, 0x12345678u, 0x30) == 1); // equal to a constant
	a.len = 0, Load(&a, var, 2);
	CHECK(Compared(o, &a, 0x12345678u, 0x31) == 0); // .. and not different from it
	a.len = 0, PushI32(&a, 0x12345678u), Load(&a, var, 2), Op(&a, 0x30);
	CHECK(Eval(o, &a) == 1); // whichever side it is on
	a.len = 0, Load(&a, var, 2);
	CHECK(Compared(o, &a, 0, 0x30) == 0); // but not to 0
	a.len = 0, Load(&a, var, 2);
	CHECK(Compared(o, &a, 0xffffffffu, 0x31) == 1); // .. or -1
	a.len = 0, Load(&a, var, 2), Load(&a, var, 2), Op(&a, 0x30);
	CHECK(Eval(o, &a) == 1); // two of them compare as they are
	a.len = 0, Load(&a, var, 2), Load(&a, var, 2), PushI32(&a, 1), Op(&a, 0x20), Op(&a, 0x30);
	CHECK(Eval(o, &a) == 0);
	a.len = 0, Load(&a, var, 2);
	CHECK(Compared(o, &a, h, 0x34) == 0); // "34" (less than) as it is
	a.len = 0, Load(&a, buf + 3, 0);
	CHECK(Compared(o, &a, 0x7777, 0x30) == 1); // a byte of the file by itself
	a.len = 0, PushI32(&a, 0), PushI32(&a, path), A8(&a, 0x80), A8(&a, 0x35);
	CHECK(Compared(o, &a, 708608, 0x30) == 1); // the size
	a.len = 0, PushI32(&a, 0), PushI32(&a, path), A8(&a, 0x80), A8(&a, 0x35);
	CHECK(Compared(o, &a, 0, 0x30) == 0); // .. which is not 0
	a.len = 0, PushI32(&a, 5);
	CHECK(Compared(o, &a, 6, 0x30) == 0); // nothing else changes

	// what an engine service writes over a variable or the buffer is not marked
	v = h ^ 0x55u;
	memcpy(d + 0x180, &v, 4);
	a.len = 0, Load(&a, var, 2);
	CHECK(Compared(o, &a, 0x12345678u, 0x30) == 0);
	// a copy of the file's bytes is, and "63" finds them equal to others
	a.len = 0;
	PushI32(&a, buf2), PushI32(&a, buf), PushI32(&a, PROGRAM_IMAGE_SIZE);
	Op(&a, 0x60);
	Load(&a, buf2 + 7, 0);
	CHECK(Compared(o, &a, 0x7777, 0x30) == 1);
	a.len = 0, PushI32(&a, buf2 + 16), PushI32(&a, zeros), PushI32(&a, 16), Op(&a, 0x63);
	CHECK(Eval(o, &a) == 1);
	a.len = 0, PushI32(&a, var), PushI32(&a, zeros), PushI32(&a, 4), Op(&a, 0x63);
	CHECK(Eval(o, &a) == 0); // (plain bytes that differ)
	d[0x800 + 100] ^= 0xff;  // the buffer changed under it
	a.len = 0, Load(&a, buf + 3, 0);
	CHECK(Compared(o, &a, 0x7777, 0x30) == 0);

	// a block that is freed leaves nothing marked behind
	args[0] = PROGRAM_IMAGE_SIZE;
	blk = Call(o, 0x80, 0x20, args, 1);
	CHECK(blk != 0);
	args[0] = blk, args[1] = 0, args[2] = path;
	CHECK(Call(o, 0x80, 0x30, args, 3) == PROGRAM_IMAGE_SIZE);
	a.len = 0, Load(&a, blk + 20, 0);
	CHECK(Compared(o, &a, 0x7777, 0x30) == 1);
	args[0] = blk;
	CHECK(Call(o, 0x80, 0x21, args, 1) == 1);
	for(i = 0; i < FOLLOW_RUNS; i++)
		CHECK(o->vm.follow.runs[i].addr == NULL || o->vm.follow.runs[i].addr == d + 0x200);

	// from one run to the next: a variable of the global memory (tag 0) holds the sum
	h = (uint32_t)(int32_t)(int8_t)image[0] * 53 + (uint32_t)(int32_t)(int8_t)image[5];
	a.len = 0;
	Load(&a, buf2, 0);
	PushI32(&a, 53), Op(&a, 0x22);
	Load(&a, buf2 + 5, 0), Op(&a, 0x20);
	PushI32(&a, 0x80);
	Op(&a, 0x0a), A8(&a, 2);
	Load(&a, 0x80, 2);
	CHECK(Compared(o, &a, 0x12345678u, 0x30) == 1);
	memcpy(&v, o->mem.globalMem + 0x80, 4);
	CHECK(v == h);
	Follow_Keep(o); // (as a save does)
	snprintf(name, sizeof name, "global 80 %x\n", (unsigned)h);
	note = Fs_OwnRead(o, "follow.txt");
	CHECK(note && strcmp(note, name) == 0);
	free(note);
	Follow_Released(o, NULL, 0); // a new run: nothing is marked ..
	o->vm.follow.keptCount = 0, o->vm.follow.live = 0;
	a.len = 0, Load(&a, 0x80, 2);
	CHECK(Compared(o, &a, 0x12345678u, 0x30) == 0);
	Follow_Start(o); // .. until the note is read
	CHECK(o->vm.follow.keptCount == 1 && o->vm.follow.live == 1);
	a.len = 0, Load(&a, 0x80, 2);
	CHECK(Compared(o, &a, 0x12345678u, 0x30) == 1); // the number a save brought back
	a.len = 0, Load(&a, 0x84, 2);
	CHECK(Compared(o, &a, 0x12345678u, 0x30) == 0); // (another variable)
	v = h + 1;
	memcpy(o->mem.globalMem + 0x80, &v, 4);
	a.len = 0, Load(&a, 0x80, 2);
	CHECK(Compared(o, &a, 0x12345678u, 0x30) == 0); // another number there is not
}

int main(void)
{
	OpenBGI_t* o = OpenBGI_New(OS_Host()); // the engine whose code the test runs
	if(!o)
		return 1;
	TestHost_SetEnv("BGI_MSGBOX_STDERR", "1"); // error boxes go to stderr instead of a dialog
	OS_Init();
	Vm_TablesInit(o);
	gT = Thread_New(o, 0x100, 0x1000, 0x1000); // 0x100 stack entries, 0x1000 bytes of code and of data
	CHECK(gT != NULL && gT->valid);
	TestPush(o);
	TestArith(o);
	TestMemory(o);
	TestStrings(o);
	TestControl(o);
	TestHeap(o);
	TestErrors(o);
	TestExtensions(o);
	TestLaterBuilds(o);
	TestBorrower(o);
	TestFiles(o);
	TestLauncher(o);
	TestProgramFile(o);
	Thread_Delete(o, gT);
	OS_Shutdown();
	if(gFails)
	{
		printf("%d failure(s)\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
