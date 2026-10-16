/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * scncmds.c - the command table of a game, read from its sysprg.arc
 *             (interface in scn.h)
 *
 * The scenario interpreter of a 1.494+ game is written in the engine's
 * own script language: scrctrl._bp fills a dispatch table in global memory
 * with one handler per base token and exports a few helpers (pop, push,
 * pop-and-resolve, range check ..) through a vector table; the other
 * scenario programs (scrsys, scrmsg, scrslct, scrsnd, scrgrp1..) add the
 * game's commands to the same dispatch table and call the helpers through
 * the vectors.  This file disassembles those programs (Asm_Decode) and
 * recovers, for every token, the module that registers it, the handler,
 * how many arguments it pops, whether it pushes a result, and the
 * parameter names its range checks print - everything the decompiler
 * needs to know about a game's commands.
 */
#include "scn_internal.h"
#include "bgi/arcread.h"
#include "bgi/asm.h"
#include "bgi/os.h"
#include "bgi/msg.h"

// the base opcodes of the engine's bytecode this scan recognizes
#define OP_PUSH_I8    0x00
#define OP_PUSH_I16   0x01
#define OP_PUSH_I32   0x02
#define OP_LOCAL_ADDR 0x04
#define OP_CODE_ADDR  0x05
#define OP_CODE_OFF   0x06
#define OP_LOAD       0x08
#define OP_STORE_KEEP 0x09
#define OP_STORE      0x0a
#define OP_PUSH_FP    0x10
#define OP_SET_FP     0x11
#define OP_JMP        0x14
#define OP_JCC        0x15
#define OP_CALL       0x16
#define OP_RET        0x17
#define OP_ADD        0x20
#define OP_MUL        0x22
#define OP_SHL        0x29

// the roles of the helper vectors scrctrl exports, in vector order
enum HelperRole
{
	H_POP = 0,
	H_PUSH,
	H_PPOP, // pop and resolve a pointer
	H_RESOLVE,
	H_POP2,
	H_PPOP2,
	H_RANGECHK,
	H_NULLCHK,
	H_ERRFMT,
	H_COUNT
};

// a disassembled program
typedef struct Prog
{
	char name[0x61];
	uint8_t* area;
	uint32_t size;
	AsmInsn_t* insn; // the instructions in order
	uint32_t count;
	uint32_t* indexOf; // offset -> instruction index (UINT32_MAX between instructions)
} Prog_t;

static int Immediate(const AsmInsn_t* in, int64_t* v)
{
	if(in->fam == 0 && (in->op == OP_PUSH_I8 || in->op == OP_PUSH_I16 || in->op == OP_PUSH_I32))
	{
		*v = in->v[0];
		return 1;
	}
	return 0;
}

static int Is(const AsmInsn_t* in, int op)
{
	return in->fam == 0 && in->op == op;
}

/* decode the whole program linearly from its entry (the scenario programs
 * are straight code followed by their strings; decoding stops at the first
 * undefined opcode, which is where the data begins) */
static int Disassemble(Prog_t* p)
{
	uint32_t off = 0, cap = 1024;
	p->insn = (AsmInsn_t*)malloc(cap * sizeof *p->insn);
	p->indexOf = (uint32_t*)malloc((p->size + 1) * sizeof *p->indexOf);
	memset(p->indexOf, 0xff, (p->size + 1) * sizeof *p->indexOf);
	p->count = 0;
	while(off < p->size)
	{
		AsmInsn_t in;
		if(!Asm_Decode(p->area, p->size, off, GEN_COUNT, &in))
			break;
		if(p->count == cap)
		{
			cap *= 2;
			p->insn = (AsmInsn_t*)realloc(p->insn, cap * sizeof *p->insn);
		}
		p->indexOf[off] = p->count;
		p->insn[p->count++] = in;
		off += in.len;
	}
	return p->count > 0;
}

static void FreeProg(Prog_t* p)
{
	free(p->area);
	free(p->insn);
	free(p->indexOf);
}

// load every "*._bp" entry of the archive whose name starts with "scr" plus script._bp
static int LoadPrograms(ArcRead_t* a, Prog_t** out, int* count)
{
	uint32_t i;
	int n = 0;
	*out = (Prog_t*)calloc(a->count + 1, sizeof **out);
	for(i = 0; i < a->count; i++)
	{
		const ArcEntry_t* e = &a->entries[i];
		uint32_t size;
		int dsc;
		uint8_t* raw;
		Prog_t* p;
		if(strncmp(e->name, "scr", 3) != 0 || strstr(e->name, "._bp") == NULL)
			continue;
		raw = ArcRead_Load(a, e, &size, &dsc);
		if(!raw || size < 16)
		{
			BGI_Free(raw);
			continue;
		}
		p = &(*out)[n];
		strcpy(p->name, e->name);
		{
			char* dot = strstr(p->name, "._bp");
			*dot = 0;
		}
		// the program file: a 16-byte header (code offset 0x10, size), then the area
		p->size = size - 16;
		p->area = (uint8_t*)malloc(p->size + 1);
		memcpy(p->area, raw + 16, p->size);
		BGI_Free(raw);
		if(Disassemble(p))
			n++;
		else
			FreeProg(p);
	}
	*count = n;
	return 1;
}

/* Find the registrations `push TABLE; push N; (push_i8 2; shl | push_i8
 * 4; mul); add; push_code_off L; store_keep 2` in a program.  With
 * `table` 0 the first registration decides the table address (returned);
 * otherwise only registrations into `table` count.  The found (token,
 * handler offset) pairs are appended to `t`. */
static uint32_t FindRegistrations(const Prog_t* p, uint32_t table, ScnCmdTable_t* t, int* cap)
{
	uint32_t i;
	for(i = 0; i + 6 < p->count; i++)
	{
		const AsmInsn_t* in = &p->insn[i];
		int64_t addr, n, k;
		if(!Immediate(in, &addr) || !Immediate(&in[1], &n) || !Immediate(&in[2], &k) || !((k == 2 && Is(&in[3], OP_SHL)) || (k == 4 && Is(&in[3], OP_MUL))) || !Is(&in[4], OP_ADD) || !Is(&in[5], OP_CODE_OFF) || !Is(&in[6], OP_STORE_KEEP) || in[6].v[0] != 2)
			continue;
		if(table == 0 && addr > 0x100)
			table = (uint32_t)addr;
		if((uint32_t)addr != table || n < 0 || n > 0xffff)
			continue;
		if(t->count == *cap)
		{
			*cap = *cap ? *cap * 2 : 256;
			t->cmds = (ScnCmd_t*)realloc(t->cmds, (size_t)*cap * sizeof *t->cmds);
		}
		memset(&t->cmds[t->count], 0, sizeof t->cmds[0]);
		t->cmds[t->count].token = (uint32_t)n;
		t->cmds[t->count].handler = in[5].target;
		snprintf(t->cmds[t->count].module, sizeof t->cmds[0].module, "%s", p->name);
		t->count++;
	}
	return table;
}

/* The helper vector table: after its registrations scrctrl stores its
 * helpers into consecutive dwords (`push_i32 V; push_code_off S;
 * store_keep 2`).  Returns the address of the first vector and fills
 * `subs[]` with the helper offsets, or 0 when the pattern is missing. */
static uint32_t FindHelpers(const Prog_t* p, uint32_t subs[H_COUNT])
{
	uint32_t i, base = 0;
	int k = 0;
	for(i = 0; i + 2 < p->count; i++)
	{
		const AsmInsn_t* in = &p->insn[i];
		int64_t v;
		if(Immediate(in, &v) && Is(&in[1], OP_CODE_OFF) && Is(&in[2], OP_STORE_KEEP) && in[2].v[0] == 2)
		{ // (a 16-bit immediate in the 1.66 game, whose globals lie lower)
			if(k == 0)
				base = (uint32_t)v;
			else if((uint32_t)v != base + 4 * (uint32_t)k)
			{
				if(k >= H_COUNT)
					break;
				k = 0;
				base = (uint32_t)v;
			}
			if(k < H_COUNT)
				subs[k] = in[1].target;
			k++;
			i += 2;
		}
	}
	return k >= H_COUNT ? base : 0;
}

/* The role a call plays: `push_code_off S; call` with S one of scrctrl's
 * helpers (in scrctrl itself), or `push_i32 V+4k; load 2; call` (the
 * other modules).  Returns the role, or -1 and leaves `*skip` 0 when the
 * instructions at i are not a helper call; `*skip` is the number of
 * instructions the call spans. */
static int HelperCall(const Prog_t* p, uint32_t i, int isCtrl, uint32_t vecBase, const uint32_t subs[H_COUNT], int* skip)
{
	const AsmInsn_t* in = &p->insn[i];
	int k;
	*skip = 0;
	if(isCtrl && i + 1 < p->count && Is(in, OP_CODE_OFF) && Is(&in[1], OP_CALL))
	{
		for(k = 0; k < H_COUNT; k++)
			if(subs[k] == in->target)
			{
				*skip = 2;
				return k;
			}
		return -1;
	}
	{
		int64_t v;
		if(i + 2 < p->count && Immediate(in, &v) && Is(&in[1], OP_LOAD) && Is(&in[2], OP_CALL) && (uint32_t)v >= vecBase && (uint32_t)v < vecBase + 4 * H_COUNT && (((uint32_t)v - vecBase) & 3) == 0)
		{
			*skip = 3;
			return (int)(((uint32_t)v - vecBase) / 4);
		}
	}
	return -1;
}

static int IsPopRole(int r)
{
	return r == H_POP || r == H_PPOP || r == H_POP2 || r == H_PPOP2;
}

/* Examine one handler: its arguments are the pops (through the helpers)
 * before its first push - the handlers take everything off the stack
 * first, sometimes converting a value through another service in between
 * or in a subroutine of their own; a push anywhere up to its `ret` means a
 * result; the strings pushed right before a range or NULL check name the
 * parameters. */
static void ExamineHandler(const Prog_t* p, int isCtrl, uint32_t vecBase, const uint32_t subs[H_COUNT], ScnCmd_t* c)
{
	uint32_t i = p->indexOf[c->handler];
	int inArgs = 1, args = 0;
	size_t plen = 0;
	if(c->handler >= p->size || i == UINT32_MAX)
	{
		c->args = -1;
		return;
	}
	for(; i < p->count; i++)
	{
		const AsmInsn_t* in = &p->insn[i];
		int skip, role = HelperCall(p, i, isCtrl, vecBase, subs, &skip);
		if(role >= 0)
		{
			if(role == H_PUSH)
			{
				c->result = 1;
				inArgs = 0;
			}
			if(IsPopRole(role) && inArgs)
				args++;
			i += (uint32_t)skip - 1;
			continue;
		}
		if(Is(in, OP_RET))
			break;
		/* a subroutine of the module: while the arguments are taken its
		 * leading pops count as arguments; a push anywhere in it (up to its
		 * ret) is the handler's result */
		if(!isCtrl && i + 1 < p->count && Is(in, OP_CODE_OFF) && Is(&in[1], OP_CALL) && in->target < p->size && p->indexOf[in->target] != UINT32_MAX)
		{
			uint32_t j;
			int subArgs = 1;
			for(j = p->indexOf[in->target]; j < p->count; j++)
			{
				int sk, r = HelperCall(p, j, isCtrl, vecBase, subs, &sk);
				if(r >= 0)
				{
					if(IsPopRole(r) && subArgs && inArgs)
						args++;
					else if(r == H_PUSH)
					{
						c->result = 1;
						subArgs = 0;
					}
					j += (uint32_t)sk - 1;
					continue;
				}
				if(Is(&p->insn[j], OP_RET))
					break;
			}
			i++;
			continue;
		}
		// a parameter name: push_code_addr STR, followed within 8 instructions by a range or NULL check
		if(Is(in, OP_CODE_ADDR) && in->target < p->size)
		{
			uint32_t j, lim = i + 9 < p->count ? i + 9 : p->count;
			for(j = i + 1; j < lim; j++)
			{
				int sk, r = HelperCall(p, j, isCtrl, vecBase, subs, &sk);
				if(r == H_RANGECHK || r == H_NULLCHK)
				{
					const char* s = (const char*)p->area + in->target;
					size_t n = strnlen(s, p->size - in->target);
					if(plen + n + 2 < sizeof c->params)
					{
						if(plen)
							c->params[plen++] = '|';
						memcpy(c->params + plen, s, n);
						plen += n;
						c->params[plen] = 0;
					}
					break;
				}
				if(r >= 0)
					break;
			}
		}
	}
	c->args = args;
}

// the names of the base tokens that the source language writes as calls
static const struct
{
	uint32_t token;
	const char* name;
} kBaseNames[] = {
	{0x40, "memcpy_r"}, // (n, src, dst) - the reversed variants the compiler's own code uses
	{0x48, "strcpy_r"}, // (src, dst)
	{0x7b, "srcinfo"},
	{0x7f, "line"},
	{0x80, "memcpy"}, // (dst, src, n)
	{0x81, "memclr"}, // (ptr, n)
	{0x82, "memset"}, // (ptr, value, n)
	{0x83, "memcmp"}, // (a, b, n) -> result
	{0x90, "strcpy"}, // (dst, src)
	{0x91, "strcat"}, // (dst, a, b)
	{0x92, "strcpy_lower"},
	{0x93, "strreplace"},
	{0x94, "strlen"},
	{0x95, "streq"},
	{0x98, "itoa"},   // (buf, value, digits)
	{0x99, "substr"}, // (dst, src, offset, count) -> result
	{0xa0, "random"},
	{0xa8, "atoi"},
	{0xaa, "bool"},
	{0xac, "mulshift"},
	{0xc0, "atan2"},
	{0xc1, "vec_len"},
	{0xe0, "set_lwork"}, // local work variables, flags, strings
	{0xe1, "lwork"},
	{0xe2, "set_gwork"},
	{0xe3, "gwork"},
	{0xe4, "set_lflag"},
	{0xe5, "lflag"},
	{0xe6, "set_gflag"},
	{0xe7, "gflag"},
	{0xe8, "lstr"},
	{0xe9, "set_gflag2"},
	{0xea, "gflag2"},
	{0xec, "regstr"},
	{0xf0, "call_scenario"},
	{0xf1, "jump_alias"},
	{0xf3, "jump_scenario"},
	{0xf4, "quit"},
	{0xf5, "quit_if_idle"},
	{0xf6, "set_label"},
	{0xf7, "test_label"},
	{0xf8, "msgbox_num"},
	{0xf9, "msgbox"},
	{0xfa, "debug_dump"},
	{0xfc, "file_dialog"},
	{0xfe, "lineinfo"},
	{0xff, "debug_pc"},
	{0, NULL},
};

/* names for the game commands that the two games examined share: a name
 * applies when the token is registered by the module and the handler's
 * first parameter name (as its range check prints it) is the one given
 * (NULL: no check) */
static const struct
{
	uint32_t token;
	const char* module;
	const char* param; // Shift-JIS (a MSG_SCN_PARAM_* macro of msg.h), or NULL
	const char* name;
} kCommandNames[] = {
	{0x118, "scrsys", NULL, "set_scene_name"},
	{0x140, "scrmsg", NULL, "msg"},
	{0x145, "scrmsg", MSG_SCN_PARAM_TEXT_SPEED, "set_text_speed"},
	{0x14c, "scrmsg", MSG_SCN_PARAM_FONT_NUMBER, "set_font"},
	{0x14d, "scrmsg", MSG_SCN_PARAM_FONT_NUMBER, "set_font_ex"},
	{0x15a, "scrmsg", MSG_SCN_PARAM_ALIAS_SOURCE, "set_name_alias"},
	{0x15d, "scrmsg", MSG_SCN_PARAM_WINDOW_DESIGN, "set_window_design"},
	{0x20e, "scrmsg", MSG_SCN_PARAM_WINDOW_MODE, "set_window_mode"},
	{0x180, "scrsnd", NULL, "bgm_play"},
	{0x184, "scrsnd", NULL, "bgm_stop"},
	{0x185, "scrsnd", NULL, "bgm_volume"},
	{0x190, "scrsnd", NULL, "amb_play"},
	{0x195, "scrsnd", NULL, "amb_volume"},
	{0x1a9, "scrsnd", NULL, "voice"},
	{0x236, "scrgrp1", MSG_SCN_PARAM_BUSTSHOT_NUMBER, "bs_shake"},
	{0x2c0, "scrgrp5", MSG_SCN_PARAM_BUSTSHOT_NUMBER, "bs_draw"},
	{0x300, "scrgrp4", MSG_SCN_PARAM_SPRITE_NUMBER, "sprite_draw"},
	{0x308, "scrgrp4", MSG_SCN_PARAM_SPRITE_NUMBER, "sprite_move"},
	{0x280, "scrgrp2", MSG_SCN_PARAM_BG_BITMAP_NAME, "bg_draw"},
	{0, NULL, NULL, NULL},
};

static int CompareCmd(const void* a, const void* b)
{
	uint32_t x = ((const ScnCmd_t*)a)->token, y = ((const ScnCmd_t*)b)->token;
	return x < y ? -1 : x > y;
}

// the module's short name for the default command names: "scrgrp1" -> "grp1", "scrmsg" -> "msg"
static void ModuleTag(const char* module, char* out, size_t n)
{
	const char* s = strncmp(module, "scr", 3) == 0 ? module + 3 : module;
	snprintf(out, n, "%s", s);
}

int Scn_ScanCommands(const char* sysprgPath, ScnCmdTable_t* out)
{
	ArcRead_t* a = ArcRead_Open(sysprgPath);
	int ok;
	memset(out, 0, sizeof *out);
	if(!a)
		return 0;
	ok = Scn_ScanCommandsArc(a, sysprgPath, out);
	ArcRead_Close(a);
	return ok;
}

int Scn_ScanCommandsArc(ArcRead_t* a, const char* sysprgPath, ScnCmdTable_t* out)
{
	Prog_t* progs = NULL;
	int count = 0, i, cap = 0, ctrl = -1;
	uint32_t table = 0, vecBase = 0, subs[H_COUNT] = {0};
	memset(out, 0, sizeof *out);
	if(!LoadPrograms(a, &progs, &count))
		return 0;
	for(i = 0; i < count; i++)
		if(strcmp(progs[i].name, "scrctrl") == 0)
			ctrl = i;
	if(ctrl < 0)
	{
		fprintf(stderr, "%s: no scrctrl._bp - not a scenario system of the 1.494+ kind\n", sysprgPath);
		goto fail;
	}
	table = FindRegistrations(&progs[ctrl], 0, out, &cap);
	if(table == 0)
	{
		fprintf(stderr, "%s: scrctrl._bp registers no token handlers\n", sysprgPath);
		goto fail;
	}
	vecBase = FindHelpers(&progs[ctrl], subs);
	for(i = 0; i < count; i++)
		if(i != ctrl)
			FindRegistrations(&progs[i], table, out, &cap);
	out->dispatch = table;
	/* the 1.64 kind: the modules register 16-bit opcodes below 0x100 (scrmsg
	 * takes 0x10 for the message), where the newer systems keep that range
	 * for the base tokens of scrctrl; its scenes are another format with a
	 * built-in opcode table (scn16.c) */
	for(i = 0; i < out->count; i++)
		if(out->cmds[i].token < 0x100 && strcmp(out->cmds[i].module, "scrctrl") != 0)
		{
			free(out->cmds);
			memset(out, 0, sizeof *out);
			out->scenes16 = 1;
			for(i = 0; i < count; i++)
				FreeProg(&progs[i]);
			free(progs);
			return 1;
		}
	if(vecBase == 0)
		fprintf(stderr, "%s: warning: the helper vectors of scrctrl._bp were not found; arguments are unknown\n", sysprgPath);
	for(i = 0; i < out->count; i++)
	{
		ScnCmd_t* c = &out->cmds[i];
		int k;
		const Prog_t* p = NULL;
		for(k = 0; k < count; k++)
			if(strcmp(progs[k].name, c->module) == 0)
				p = &progs[k];
		if(vecBase && p)
			ExamineHandler(p, p == &progs[ctrl], vecBase, subs, c);
		else
			c->args = -1;
		for(k = 0; kBaseNames[k].name; k++)
			if(kBaseNames[k].token == c->token && c->token < 0x100)
				snprintf(c->name, sizeof c->name, "%s", kBaseNames[k].name);
		for(k = 0; kCommandNames[k].name; k++)
			if(kCommandNames[k].token == c->token && strcmp(kCommandNames[k].module, c->module) == 0 && (!kCommandNames[k].param || strncmp(c->params, kCommandNames[k].param, strlen(kCommandNames[k].param)) == 0))
				snprintf(c->name, sizeof c->name, "%s", kCommandNames[k].name);
		if(!c->name[0])
		{
			char tag[16];
			ModuleTag(c->module, tag, sizeof tag);
			snprintf(c->name, sizeof c->name, "%s_%03x", tag, c->token);
		}
	}
	qsort(out->cmds, (size_t)out->count, sizeof *out->cmds, CompareCmd);
	for(i = 0; i < count; i++)
		FreeProg(&progs[i]);
	free(progs);
	return 1;
fail:
	for(i = 0; i < count; i++)
		FreeProg(&progs[i]);
	free(progs);
	Scn_FreeCommands(out);
	return 0;
}

void Scn_FreeCommands(ScnCmdTable_t* t)
{
	free(t->cmds);
	memset(t, 0, sizeof *t);
}

const ScnCmd_t* Scn_FindCommand(const ScnCmdTable_t* t, uint32_t token)
{
	int lo = 0, hi = t ? t->count - 1 : -1;
	while(lo <= hi)
	{
		int mid = (lo + hi) / 2;
		if(t->cmds[mid].token == token)
			return &t->cmds[mid];
		if(t->cmds[mid].token < token)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}

const ScnCmd_t* Scn_FindCommandByName(const ScnCmdTable_t* t, const char* name, size_t len)
{
	int i;
	if(!t)
		return NULL;
	for(i = 0; i < t->count; i++)
		if(strlen(t->cmds[i].name) == len && memcmp(t->cmds[i].name, name, len) == 0)
			return &t->cmds[i];
	return NULL;
}

void Scn_WriteCommands(const ScnCmdTable_t* t, FILE* out)
{
	int i;
	if(t->scenes16)
	{
		Scn16_WriteOps(out);
		return;
	}
	fprintf(out, "; dispatch table at 0x%x, %d commands\n", t->dispatch, t->count);
	fprintf(out, "; token  module    name                 args result handler  parameters\n");
	for(i = 0; i < t->count; i++)
	{
		const ScnCmd_t* c = &t->cmds[i];
		char utf[sizeof c->params * 3 + 1];
		OS_SjisToUtf8(c->params, utf, sizeof utf);
		fprintf(out, "  0x%03x  %-9s %-20s %4d %6d  0x%04x   %s\n", c->token, c->module, c->name, c->args, c->result, c->handler, utf);
	}
}
