/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * scntok.c - the names of the base tokens and the operator table
 *            (scn_internal.h)
 */
#include "scn_internal.h"

static const struct
{
	uint32_t token;
	const char* name;
} kNames[] = {
	{SCN_PUSH, "push"},
	{SCN_ADDR, "addr"},
	{SCN_LOCAL, "local"},
	{SCN_STR, "str"},
	{SCN_LOAD, "load"},
	{SCN_STORE, "store"},
	{SCN_STORE2, "store2"},
	{SCN_PUSHFP, "push_fp"},
	{SCN_SETFP, "set_fp"},
	{SCN_JMP, "jmp"},
	{SCN_JCC, "jcc"},
	{SCN_CALL, "call"},
	{SCN_RET, "ret"},
	{SCN_CALLFN, "callfn"},
	{SCN_SETRET, "setret"},
	{SCN_TRY, "try"},
	{SCN_ENDTRY, "endtry"},
	{SCN_ADD, "add"},
	{SCN_SUB, "sub"},
	{SCN_MUL, "mul"},
	{SCN_DIV, "div"},
	{SCN_MOD, "mod"},
	{SCN_AND, "and"},
	{SCN_OR, "or"},
	{SCN_XOR, "xor"},
	{SCN_NOT, "not"},
	{SCN_SHL, "shl"},
	{SCN_SHR, "shr"},
	{SCN_SAR, "sar"},
	{SCN_EQ, "eq"},
	{SCN_NE, "ne"},
	{SCN_LE, "le"},
	{SCN_GE, "ge"},
	{SCN_LT, "lt"},
	{SCN_GT, "gt"},
	{SCN_LAND, "land"},
	{SCN_LOR, "lor"},
	{SCN_LNOT, "lnot"},
	{SCN_ARGS, "args"},
	{SCN_SRCINFO, "srcinfo"},
	{SCN_LINE, "line"},
	{0, NULL},
};

const char* ScnTok_Name(uint32_t token)
{
	int i;
	for(i = 0; kNames[i].name; i++)
		if(kNames[i].token == token)
			return kNames[i].name;
	return NULL;
}

uint32_t ScnTok_Lookup(const char* name, size_t len)
{
	int i;
	for(i = 0; kNames[i].name; i++)
		if(strlen(kNames[i].name) == len && memcmp(kNames[i].name, name, len) == 0)
			return kNames[i].token;
	return UINT32_MAX;
}

const ScnOperator_t kScnBinaryOps[] = {
	{SCN_MUL, "*", 13},
	{SCN_DIV, "/", 13},
	{SCN_MOD, "%", 13},
	{SCN_ADD, "+", 12},
	{SCN_SUB, "-", 12},
	{SCN_SHL, "<<", 11},
	{SCN_SAR, ">>", 11},
	{SCN_SHR, ">>>", 11},
	{SCN_LT, "<", 10},
	{SCN_LE, "<=", 10},
	{SCN_GT, ">", 10},
	{SCN_GE, ">=", 10},
	{SCN_EQ, "==", 9},
	{SCN_NE, "!=", 9},
	{SCN_AND, "&", 8},
	{SCN_XOR, "^", 7},
	{SCN_OR, "|", 6},
	{SCN_LAND, "&&", 5},
	{SCN_LOR, "||", 4},
};
const int kScnBinaryOpCount = (int)(sizeof kScnBinaryOps / sizeof kScnBinaryOps[0]);

const ScnOperator_t* ScnOp_ByToken(uint32_t token)
{
	int i;
	for(i = 0; i < kScnBinaryOpCount; i++)
		if(kScnBinaryOps[i].token == token)
			return &kScnBinaryOps[i];
	return NULL;
}
