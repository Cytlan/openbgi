/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bpc_internal.h - what the program decompiler (bpdec.c) and compiler
 *                  (bpcomp.c) share: the operator table and the shapes of
 *                  the original compiler's output (docs/bpc.md)
 *
 * The shapes, as both sides rely on them:
 *   function   push_fp; push F; add; set_fp; (push_local_addr n; store 2)*
 *              for the parameters (the first parameter is popped first),
 *              the body, then EXIT: push_fp; push F; sub; set_fp; ret.
 *              The frame holds the parameters and the locals in
 *              declaration order from its lowest address: a variable at
 *              frame offset o is `push_local_addr F - o`.
 *   call       the arguments right to left, `push_code_off f; call` (or
 *              an address expression and `call`); engine instructions
 *              take their arguments left to right; sprintf takes the
 *              conversion arguments right to left, then dst, then fmt.
 *   assignment lhs-address; value; store_keep (the value stays on the
 *              stack - every expression statement leaves its value) -
 *              but value; lhs-address; store when the value is a call
 *              or a declaration's initializer.
 *   immediate  push_i8 for 0..127, push_i16 up to 32767, else push_i32;
 *              a negative constant is `-x`: x; not; push_i8 1; add.
 *   condition  `&&`, `||` and `!` at the top of a condition are chains
 *              of conditional jumps (parentheses make a value, computed
 *              with land / lor / lnot); a leaf is `value; jcc c` with
 *              c 1 (jump when zero) to the false target or 0 to the
 *              true target, whichever is not the fall-through.
 *   if         cond(c, NEXT, ELSE); then; [push_code_off END; jmp;
 *              ELSE: else;] END: - an `if` whose statement is a single
 *              break / continue / goto / return jumps there directly:
 *              cond(c, TARGET, NEXT).
 *   while      HEAD: cond(c, NEXT, END); body; push_code_off HEAD; jmp; END:
 *   do-while   HEAD: body; cond(c, HEAD, NEXT)
 *   return     [value;] push_code_off EXIT; jmp
 *   strings    after the code, in order of appearance, each occurrence
 *              its own copy; the code is padded to 16 bytes before them
 *              and the area to 16 bytes after them.
 *
 * What only the compiler knows (the decompiler writes neither):
 *   variable   `global int g @ A;` declares a name for an address and
 *              emits nothing.  The name is the address as the
 *              declaration wrote it, unfolded (`@ 100 + 4`: push_i8 100;
 *              push_i8 4; add), followed by `load s` where a value is
 *              read; the areas other than `global` put the tag of the
 *              engine's pointer layout on the first number, a push_i32.
 *   element    a[i]: the array's address; i; then `push S; mul` for
 *              elements of S = 2 or 4 bytes; add - and `load s` for the
 *              value.  &a[i] is the same without the load.
 */
#ifndef BPC_INTERNAL_H_
#define BPC_INTERNAL_H_

#include "bgi/bpc.h"
#include "bgi/os.h"

// the binary operators: base opcode, source spelling and precedence (higher binds tighter)
typedef struct BpcOperator
{
	int op;
	const char* text;
	int prec;
} BpcOperator_t;
extern const BpcOperator_t kBpcBinaryOps[];
extern const int kBpcBinaryOpCount;
const BpcOperator_t* BpcOp_ByOpcode(int op);
const BpcOperator_t* BpcOp_ByText(const char* text, size_t len);

// precedence levels the printer and the parser share
#define BPC_PREC_ASSIGN  2 // = and <-
#define BPC_PREC_SELECT  3 // ?:
#define BPC_PREC_LOR     4
#define BPC_PREC_LAND    5
#define BPC_PREC_UNARY   14
#define BPC_PREC_PRIMARY 15

// the base opcodes the shapes name
#define OP_PUSH_I8       0x00
#define OP_PUSH_I16      0x01
#define OP_PUSH_I32      0x02
#define OP_LOCAL_ADDR    0x04
#define OP_CODE_ADDR     0x05
#define OP_CODE_OFF      0x06
#define OP_LOAD          0x08
#define OP_STORE_KEEP    0x09
#define OP_STORE         0x0a
#define OP_STORE_INL     0x0b
#define OP_STORE_MUL     0x0c
#define OP_PUSH_FP       0x10
#define OP_SET_FP        0x11
#define OP_JMP           0x14
#define OP_JCC           0x15
#define OP_CALL          0x16
#define OP_RET           0x17
#define OP_ADD           0x20
#define OP_SUB           0x21
#define OP_NOT           0x28
#define OP_LNOT          0x3a
#define OP_SELECT        0x40
#define OP_SPRINTF       0x6f

#endif
