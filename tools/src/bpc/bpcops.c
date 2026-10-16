/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bpcops.c - the operator table of the program decompiler and compiler
 *            (bpc_internal.h)
 */
#include "bpc_internal.h"

const BpcOperator_t kBpcBinaryOps[] = {
	{0x22, "*", 13},
	{0x23, "/", 13},
	{0x24, "%", 13},
	{0x20, "+", 12},
	{0x21, "-", 12},
	{0x29, "<<", 11},
	{0x2b, ">>", 11},  // sar: the arithmetic shift is the C operator
	{0x2a, ">>>", 11}, // shr: the logical shift
	{0x34, "<", 10},
	{0x32, "<=", 10},
	{0x35, ">", 10},
	{0x33, ">=", 10},
	{0x30, "==", 9},
	{0x31, "!=", 9},
	{0x25, "&", 8},
	{0x27, "^", 7},
	{0x26, "|", 6},
	{0x38, "&&", 5},
	{0x39, "||", 4},
};
const int kBpcBinaryOpCount = (int)(sizeof kBpcBinaryOps / sizeof kBpcBinaryOps[0]);

const BpcOperator_t* BpcOp_ByOpcode(int op)
{
	int i;
	for(i = 0; i < kBpcBinaryOpCount; i++)
		if(kBpcBinaryOps[i].op == op)
			return &kBpcBinaryOps[i];
	return NULL;
}

const BpcOperator_t* BpcOp_ByText(const char* text, size_t len)
{
	int i;
	for(i = 0; i < kBpcBinaryOpCount; i++)
		if(strlen(kBpcBinaryOps[i].text) == len && memcmp(kBpcBinaryOps[i].text, text, len) == 0)
			return &kBpcBinaryOps[i];
	return NULL;
}
