/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * scn16.c - the scenes of the 1.64 games (NurseryRhyme, nrarc02.arc): a
 *           flat stream of 16-bit opcodes with inline operands, their
 *           decoder, decompiler and compiler (interface in scn_internal.h;
 *           the source language in docs/scenario.md)
 *
 * The scene interpreter of these games (scrmain._bp) fetches a 16-bit
 * opcode and calls the handler the dispatch table holds for it; the
 * handler reads its own operands from the stream - dwords and NUL-
 * terminated strings - through two helpers of scrdrv._bp, so every
 * opcode's operand list is fixed by its handler (kOps below, read from
 * the decompiled handlers of scrmsg, scrgrp, scrgrp2, scrsnd, scrsys,
 * scrctrl, scrslct and anmscr: internal/re_tools/scn16sig.py lists the
 * reads of every registered handler, the meaning of each dword was read
 * from the handler by hand).  There is no stack and no expression: a
 * jump carries its target as a file offset, a message carries the file
 * offset of its text, and the texts lie after the code in order of
 * reference, from the next multiple of 16 (zero padding between; a file
 * without a message ends with its code).  A variable operand is a dword
 * whose high word selects the array (1: the scene's work variables, 2:
 * the global ones) and whose low word is the index; a value operand with
 * a zero high word is a literal.
 *
 * The source is one statement per line - `name(args);`, with labels for
 * the jump targets and the control opcodes in a C-like spelling (`goto`,
 * `if (lwork(1) == 5) goto L;`, `lwork(1) = 0;`, `call`, `return`, `jump
 * "file";`, `end;`) - and compiles back to the identical bytes, which the
 * decompiler verifies before it writes.
 */
#include "scn_internal.h"
#include "bgi/asm.h"

#include <ctype.h>
#include <stdarg.h>

// ---- the opcodes ------------------------------------------------------------------------------------

/* The operand signature letters: d a dword, s an inline string, t the
 * offset of a text (a dword), l a code offset (a dword), v a variable, x a
 * variable or a literal; D / S / L a dword count followed by that many
 * dwords / strings / labels (at least one: the handlers' loops run once
 * before they test the count).  The names are this implementation's,
 * after what the handlers do; an opcode nothing is known about is named
 * after its module and number like the scan does for the newer games. */
static const Scn16Op_t kOps[] = {
	{0x10, "msg", "ddt"},                 // no-wait flag, end-of-message flag, the text
	{0x11, "msg_clear", ""},              // wait for a click and clear the window
	{0x12, "kana", "ss"},                 // register a reading for a name (name, reading)
	{0x13, "kana_remove", "s"},           // forget it
	{0x14, "name", "s"},                  // the speaker of the next message
	{0x18, "set_font", "ddddd"},          // the message font (kind, size, ratio, spacing, line spacing)
	{0x19, "set_text_style", "dddd"},     // outline or shadow: on, x, y, strength
	{0x1a, "set_text_color", "ddd"},      // r, g, b
	{0x1b, "set_name_color", "sddddddd"}, // a name's text and shadow colours (name, r, g, b, r, g, b, flag)
	{0x1c, "set_font2", "ddddd"},         // the alternative font, applied by use_font2
	{0x1d, "use_font2", ""},
	{0x1e, "set_name_default_color", "ddd"},
	{0x1f, "set_window_mode", "d"}, // 0: message window, 1: novel (full screen)
	{0x20, "window_hide", ""},
	{0x21, "window_show", ""},
	{0x28, "bg", "sd"},       // a background (bitmap, time)
	{0x29, "bg_mask", "ssd"}, // with a transition mask (bitmap, mask, time)
	{0x2a, "bg_hide", "d"},
	{0x2b, "bg_hide_mask", "sd"},
	{0x2c, "bg_scroll", "sdddddddd"}, // a larger background moved across the view
	{0x2d, "bg_view", "ddddd"},       // the part of it shown (x, y, w, h, time)
	{0x30, "cutin", "sd"},            // a picture centred over the scene (bitmap, time)
	{0x31, "cutin_blend", "sdd"},     // ... with an opacity
	{0x32, "cutin_hide", "d"},
	{0x33, "cutin_color", "d"},      // the shade behind it: 0 black, 1 white
	{0x34, "sprite", "dsddddd"},     // a sprite (slot, bitmap, x, y, opacity, level, time)
	{0x35, "sprite_hide", "dd"},     // slot, time
	{0x36, "sprite_move", "dddddd"}, // slot, x, y, level, flag, time (0x8000xxxx: relative)
	{0x37, "sprite_wait", "d"},
	{0x38, "snow", "dd"}, // on, time
	{0x39, "snow_count", "d"},
	{0x3a, "snow_speed", "d"},
	{0x3b, "snow_clear", ""},
	{0x3c, "quake", "ddddd"},
	{0x3d, "quake_preset", "dd"},
	{0x40, "bust", "ddsdd"},       // a bust shot (slot, position, bitmap, level, time); applies the queue
	{0x41, "bust_add", "ddsdd"},   // the same, queued until the next bust command
	{0x42, "bust_change", "ddsd"}, // cross-fade to another bitmap at a position (slot, position, bitmap, time)
	{0x43, "bust_change_add", "ddsd"},
	{0x44, "bust_pose", "ddsd"}, // a new bitmap keeping the face (slot, position, bitmap, time)
	{0x45, "bust_pose_add", "ddsd"},
	{0x46, "bust_face", "dsd"}, // cross-fade in place (slot, bitmap, time)
	{0x47, "bust_face_add", "dsd"},
	{0x48, "bust_hide", "dd"}, // slot, time
	{0x49, "bust_hide_add", "dd"},
	{0x4a, "bust_swap", "dsd"}, // replace the bitmap of a shown bust without a transition
	{0x4c, "face", "sd"},       // the face in the message window (bitmap, time)
	{0x4e, "face_hide", "d"},
	{0x50, "ev", "sd"}, // an event picture over everything (bitmap, time)
	{0x51, "ev_mask", "ssd"},
	{0x52, "ev_clear", "d"}, // to black (time)
	{0x53, "ev_clear_mask", "sd"},
	{0x54, "ev_slide", "sdd"},     // bitmap, direction, time
	{0x60, "fx_stretch", "ddddd"}, // a distortion effect: x, y, w, h, time
	{0x61, "fx_random", "dd"},     // strength, time
	{0x62, "fx_ripple", "dddddd"},
	{0x65, "fx_hide", "d"},
	{0x66, "fx_gradient", "dd"},
	{0x67, "fx_gradient_hide", "d"},
	{0x68, "fade_black", "d"}, // fade the scene out to a colour and clear it (time)
	{0x69, "fade_white", "d"},
	{0x6a, "fade_white_1", "d"},
	{0x6b, "filter_white_2", "d"}, // a colour filter over the scene (time)
	{0x6c, "filter_white_3", "d"},
	{0x6d, "filter_preset", "ddd"}, // preset, opacity, time
	{0x6e, "filter", "dddddd"},     // mode, r, g, b, opacity, time
	{0x6f, "filter_hide", "d"},
	{0x70, "bgm_play", "dsd"}, // channel, name, volume
	{0x71, "bgm_stop", "d"},
	{0x72, "bgm_volume", "ddd"}, // channel, volume, time
	{0x74, "amb_play", "dsd"},   // the ambient channels (4, 5)
	{0x75, "amb_stop", "d"},
	{0x76, "amb_volume", "ddd"},
	{0x78, "env_play", "dsd"}, // the environment channels (6, 7; silenced while skipping)
	{0x79, "env_stop", "d"},
	{0x7a, "env_volume", "ddd"},
	{0x80, "se", "sd"}, // a sound effect (name, loop)
	{0x82, "se_stop", ""},
	{0x83, "se_wait", ""},
	{0x84, "voice_ch", "dsd"}, // channel, name, start offset
	{0x85, "voice", "s"},      // the usual form: on channel 0
	{0x86, "voice_stop", "d"},
	{0x87, "voice_wait", "d"},
	{0x88, "movie", "s"},
	{0x8c, "wait", "d"}, // milliseconds (skipped in fast forward)
	{0x8d, "timer_set", "d"},
	{0x8e, "timer_wait", "d"},
	{0x8f, "key_wait", ""},
	{0x90, "flag_set", "d"}, // the scene flags (bits)
	{0x91, "flag_clear", "d"},
	{0x92, "gflag_set", "d"}, // the global flags
	{0x93, "gflag_clear", "d"},
	{0x94, "gflag_test", "d"}, // lwork(0) = the flag
	{0x98, "set", "vx"},       // lwork(1) = 5
	{0x99, "add", "vx"},       // lwork(1) += 5
	{0x9a, "sub", "vx"},
	{0x9b, "mul", "vx"},
	{0x9c, "div", "vx"},
	{0x9d, "mod", "vx"},
	{0xa0, "jump", "l"},              // goto L
	{0xa1, "jump_if_flag", "dl"},     // if (flag(n)) goto L
	{0xa2, "jump_if_not_flag", "dl"}, // if (!flag(n)) goto L
	{0xa3, "jump_eq", "vxl"},         // if (lwork(1) == 5) goto L
	{0xa4, "jump_ne", "vxl"},
	{0xa5, "jump_ge", "vxl"},
	{0xa6, "jump_gt", "vxl"},
	{0xa7, "jump_le", "vxl"},
	{0xa8, "jump_lt", "vxl"},
	{0xa9, "select_jump", "L"}, // goto the label lwork(0) selects
	{0xac, "gosub", "l"},       // call L
	{0xad, "return", ""},       // return
	{0xae, "sys_ae", "l"},      // a position the interpreter waits at while the menu is open
	{0xaf, "menu_wait", ""},
	{0xb0, "select", "S"},       // a choice: the texts; lwork(0) gets the answer
	{0xb4, "select_voice", "S"}, // the voices read for the choices
	{0xb8, "select_reset", ""},  // forget which choices were taken
	{0xb9, "select_disable", "x"},
	{0xba, "select_enable", "x"},
	{0xc0, "jump_scenario", "s"},  // jump "file"
	{0xc1, "call_scenario", "s"},  // call "file"
	{0xc2, "end", ""},             // end: back to the caller
	{0xc4, "scene", "d"},          // the scene program (titles, the chapter screens)
	{0xc8, "set_scene_name", "s"}, // the name the save data shows
	{0xc9, "flush_input", ""},
	{0xd0, "get_mode", ""},        // lwork(0) = the play mode
	{0xd4, "random", "d"},         // lwork(0) = 0 .. n
	{0xd8, "set_event_mode", "d"}, // 1: the window is away and waits are shortened
	{0xd9, "set_skip_enable", "d"},
	{0xda, "sys_da", "d"}, // the state of two menu icons
	{0xdb, "sys_db", "d"},
	{0xdc, "set_bust_visible", "d"},
	{0xe0, "animation", "s"}, // an animation of animation.arc
	{0xf8, "debug_print", "s"},
	{0xf9, "debug_print_var", "sx"},
	{0x137, "sprite_anim", "dsddd"}, // slot, bitmap base, a, b, c
	{0x138, "rain", "dd"},           // on, time
	{0x139, "rain_speed", "d"},
	{0x13a, "rain_density", "d"},
	{0x148, "char_bust", "ssddD"}, // a character's bust shots (name, bitmap base, a, b, the faces)
	{0x149, "char_bust_remove", "s"},
	{0x14c, "char_mouth", "ssddd"}, // its mouth animation (name, bitmap base, a, b, c)
	{0x14d, "char_mouth_remove", "s"},
	{0x14e, "set_name_alias", "ss"}, // name, alias
	{0x14f, "remove_name_alias", "s"},
	{0x16e, "filter_anim", "dddddd"}, // like filter, animated
};
#define OP_COUNT     ((int)(sizeof kOps / sizeof kOps[0]))

#define OP_SET       0x98
#define OP_MOD       0x9d
#define OP_JUMP      0xa0
#define OP_JUMP_FLAG 0xa1
#define OP_JUMP_NOT  0xa2
#define OP_JUMP_EQ   0xa3
#define OP_JUMP_LT   0xa8
#define OP_GOSUB     0xac
#define OP_RETURN    0xad
#define OP_JUMP_SCN  0xc0
#define OP_CALL_SCN  0xc1
#define OP_END       0xc2

static const char* const kAssignOps[] = {"=", "+=", "-=", "*=", "/=", "%="}; // 0x98 .. 0x9d
static const char* const kCompareOps[] = {"==", "!=", ">=", ">", "<=", "<"}; // 0xa3 .. 0xa8

const Scn16Op_t* Scn16_FindOp(uint32_t op)
{
	int i;
	for(i = 0; i < OP_COUNT; i++)
		if(kOps[i].op == op)
			return &kOps[i];
	return NULL;
}

const Scn16Op_t* Scn16_FindOpByName(const char* name, size_t len)
{
	int i;
	for(i = 0; i < OP_COUNT; i++)
		if(strlen(kOps[i].name) == len && memcmp(kOps[i].name, name, len) == 0)
			return &kOps[i];
	return NULL;
}

void Scn16_WriteOps(FILE* out)
{
	int i;
	fprintf(out, "; the 16-bit scene opcodes of the 1.64 games (built in: src/scn/scn16.c)\n");
	fprintf(out, "; opcode name                     operands (d dword, s string, t text, l label, v variable, x value, D/S/L a counted list)\n");
	for(i = 0; i < OP_COUNT; i++)
		fprintf(out, "  0x%03x  %-24s %s\n", kOps[i].op, kOps[i].name, kOps[i].sig);
}

// ---- the decoder ------------------------------------------------------------------------------------

static uint32_t Rd32(const uint8_t* p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int Fail(char* err, size_t errSize, const char* fmt, ...)
{
	va_list ap;
	if(err && errSize)
	{
		va_start(ap, fmt);
		vsnprintf(err, errSize, fmt, ap);
		va_end(ap);
	}
	return 0;
}

/* Decode the instruction at `off`; the strings must end before `size`.
 * Returns 1 and fills `out`, or 0 with a message. */
static int DecodeOne(const uint8_t* data, uint32_t size, uint32_t off, Scn16Insn_t* out, char* err, size_t errSize)
{
	const char* sig;
	uint32_t p = off;
	memset(out, 0, sizeof *out);
	out->off = off;
	if(off + 2 > size)
		return Fail(err, errSize, "the code runs past the end of the file at 0x%x", off);
	out->op = (uint16_t)(data[off] | (data[off + 1] << 8));
	out->def = Scn16_FindOp(out->op);
	if(!out->def)
		return Fail(err, errSize, "undefined opcode 0x%x at 0x%x", out->op, off);
	p += 2;
	for(sig = out->def->sig; *sig; sig++)
	{
		char c = *sig;
		uint32_t n = 1, k;
		if(c == 'D' || c == 'S' || c == 'L')
		{ // a count, then that many (at least one)
			if(p + 4 > size)
				return Fail(err, errSize, "the operands of 0x%x at 0x%x run past the end of the file", out->op, off);
			n = Rd32(data + p);
			if(out->argc >= SCN16_MAX_ARGS)
				return Fail(err, errSize, "too many operands at 0x%x", off);
			out->args[out->argc].kind = 'n';
			out->args[out->argc].num = n;
			out->argc++;
			p += 4;
			if(n == 0)
				n = 1;
			if(n > SCN16_MAX_ARGS)
				return Fail(err, errSize, "too many operands at 0x%x", off);
			if(c == 'D')
				c = 'd';
			else if(c == 'S')
				c = 's';
			else
				c = 'l';
		}
		for(k = 0; k < n; k++)
		{
			Scn16Arg_t* a = &out->args[out->argc];
			if(out->argc >= SCN16_MAX_ARGS)
				return Fail(err, errSize, "too many operands at 0x%x", off);
			a->kind = c;
			if(c == 's')
			{
				uint32_t e = p;
				while(e < size && data[e])
					e++;
				if(e >= size)
					return Fail(err, errSize, "an unterminated string at 0x%x", p);
				a->str = (const char*)data + p;
				a->num = p;
				p = e + 1;
			}
			else
			{
				if(p + 4 > size)
					return Fail(err, errSize, "the operands of 0x%x at 0x%x run past the end of the file", out->op, off);
				a->num = Rd32(data + p);
				p += 4;
			}
			out->argc++;
		}
	}
	out->len = p - off;
	return 1;
}

int Scn16_Parse(const uint8_t* data, uint32_t size, Scn16Scene_t* out, char* err, size_t errSize)
{
	uint32_t off = 0, textStart = size, i;
	memset(out, 0, sizeof *out);
	out->data = data;
	out->size = size;
	if(size < 2)
		return Fail(err, errSize, "not a 16-bit scene");
	while(off < textStart)
	{
		Scn16Insn_t in;
		int k;
		if(textStart - off < 16)
		{ // the zero padding before the texts
			for(i = off; i < textStart && !data[i]; i++)
				;
			if(i == textStart)
				break;
		}
		if(!DecodeOne(data, size, off, &in, err, errSize))
		{
			free(out->insns);
			out->insns = NULL;
			return 0;
		}
		for(k = 0; k < in.argc; k++)
			if(in.args[k].kind == 't')
			{
				uint32_t t = in.args[k].num;
				if(t >= size || t < off + in.len)
				{
					free(out->insns);
					out->insns = NULL;
					return Fail(err, errSize, "a text offset 0x%x out of range at 0x%x", t, off);
				}
				if(t < textStart)
					textStart = t;
			}
		if(out->count == out->cap)
		{
			out->cap = out->cap ? out->cap * 2 : 256;
			out->insns = (Scn16Insn_t*)realloc(out->insns, out->cap * sizeof *out->insns);
		}
		out->insns[out->count++] = in;
		off += in.len;
	}
	out->codeEnd = off;
	out->textStart = textStart;
	if(out->count == 0)
		return Fail(err, errSize, "not a 16-bit scene");
	// the layout the compiler of the games used: the texts from the next multiple of 16 after the code
	if(textStart != size && (textStart < off || textStart - off > 15 || (textStart & 15)))
	{
		free(out->insns);
		out->insns = NULL;
		return Fail(err, errSize, "the texts do not start at the next multiple of 16 after the code");
	}
	for(i = off; i < textStart; i++)
		if(data[i])
		{
			free(out->insns);
			out->insns = NULL;
			return Fail(err, errSize, "non-zero padding before the texts");
		}
	return 1;
}

void Scn16_FreeScene(Scn16Scene_t* s)
{
	free(s->insns);
	memset(s, 0, sizeof *s);
}

int Scn16_Check(const uint8_t* data, uint32_t size)
{
	Scn16Scene_t s;
	if(!Scn16_Parse(data, size, &s, NULL, 0))
		return 0;
	Scn16_FreeScene(&s);
	return 1;
}

// ---- the decompiler -----------------------------------------------------------------------------------

static const Scn16Insn_t* InsnAt(const Scn16Scene_t* s, uint32_t off)
{
	uint32_t lo = 0, hi = s->count;
	while(lo < hi)
	{
		uint32_t mid = (lo + hi) / 2;
		if(s->insns[mid].off < off)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < s->count && s->insns[lo].off == off ? &s->insns[lo] : NULL;
}

// a code offset is a label when an instruction starts there (or the code ends there)
static int IsLabel(const Scn16Scene_t* s, uint32_t off)
{
	return off == s->codeEnd || InsnAt(s, off) != NULL;
}

static void PutNumber(FILE* b, uint32_t v)
{
	if(v < 0x10000 || v >= 0xffff0000u)
		fprintf(b, "%d", (int)v);
	else
		fprintf(b, "0x%08x", v);
}

static void PutVar(FILE* b, uint32_t v)
{
	if((v >> 16) == 1)
		fprintf(b, "lwork(%u)", v & 0xffff);
	else if((v >> 16) == 2)
		fprintf(b, "gwork(%u)", v & 0xffff);
	else
		PutNumber(b, v);
}

static void PutArg(FILE* b, const Scn16Scene_t* s, const Scn16Arg_t* a)
{
	switch(a->kind)
	{
		case 's':
			Asm_WriteStringLiteral(b, (const uint8_t*)a->str, (uint32_t)strlen(a->str));
			break;
		case 't':
			Asm_WriteStringLiteral(b, s->data + a->num, (uint32_t)strlen((const char*)s->data + a->num));
			break;
		case 'l':
			if(IsLabel(s, a->num))
				fprintf(b, "L_%04x", a->num);
			else
				PutNumber(b, a->num);
			break;
		case 'v':
		case 'x':
			PutVar(b, a->num);
			break;
		default:
			PutNumber(b, a->num);
	}
}

// the arguments of a command, the counts of the D / S / L forms left out
static void PutArgs(FILE* b, const Scn16Scene_t* s, const Scn16Insn_t* in)
{
	int k, first = 1;
	for(k = 0; k < in->argc; k++)
	{
		if(in->args[k].kind == 'n')
			continue;
		if(!first)
			fprintf(b, ", ");
		first = 0;
		PutArg(b, s, &in->args[k]);
	}
}

static void PutStatement(FILE* b, const Scn16Scene_t* s, const Scn16Insn_t* in)
{
	const Scn16Arg_t* a = in->args;
	if(in->op >= OP_SET && in->op <= OP_MOD)
	{
		PutArg(b, s, &a[0]);
		fprintf(b, " %s ", kAssignOps[in->op - OP_SET]);
		PutArg(b, s, &a[1]);
	}
	else if(in->op == OP_JUMP)
	{
		fprintf(b, "goto ");
		PutArg(b, s, &a[0]);
	}
	else if(in->op == OP_JUMP_FLAG || in->op == OP_JUMP_NOT)
	{
		fprintf(b, in->op == OP_JUMP_FLAG ? "if (flag(" : "if (!flag(");
		PutArg(b, s, &a[0]);
		fprintf(b, ")) goto ");
		PutArg(b, s, &a[1]);
	}
	else if(in->op >= OP_JUMP_EQ && in->op <= OP_JUMP_LT)
	{
		fprintf(b, "if (");
		PutArg(b, s, &a[0]);
		fprintf(b, " %s ", kCompareOps[in->op - OP_JUMP_EQ]);
		PutArg(b, s, &a[1]);
		fprintf(b, ") goto ");
		PutArg(b, s, &a[2]);
	}
	else if(in->op == OP_GOSUB || in->op == OP_JUMP_SCN || in->op == OP_CALL_SCN)
	{
		fprintf(b, in->op == OP_JUMP_SCN ? "jump " : "call ");
		PutArg(b, s, &a[0]);
	}
	else if(in->op == OP_RETURN)
		fprintf(b, "return");
	else if(in->op == OP_END)
		fprintf(b, "end");
	else
	{
		fprintf(b, "%s(", in->def->name);
		PutArgs(b, s, in);
		fprintf(b, ")");
	}
	fprintf(b, ";\n");
}

// mark every code offset a jump refers to
static uint8_t* FindLabels(const Scn16Scene_t* s)
{
	uint8_t* marks = (uint8_t*)calloc(s->codeEnd + 1, 1);
	uint32_t i;
	int k;
	for(i = 0; i < s->count; i++)
		for(k = 0; k < s->insns[i].argc; k++)
			if(s->insns[i].args[k].kind == 'l' && IsLabel(s, s->insns[i].args[k].num))
				marks[s->insns[i].args[k].num] = 1;
	return marks;
}

static void WriteSource(FILE* b, const Scn16Scene_t* s, const char* name)
{
	uint8_t* marks = FindLabels(s);
	uint32_t i;
	fprintf(b, "// %s - decompiled by bgiscn (docs/scenario.md)\n#style inline\n%s\n", name ? name : "scene",
		gAsmUtf8 ? "#encoding utf-8\n" : "");
	for(i = 0; i < s->count; i++)
	{
		const Scn16Insn_t* in = &s->insns[i];
		if(marks[in->off])
			fprintf(b, "L_%04x:\n", in->off);
		PutStatement(b, s, in);
	}
	if(marks[s->codeEnd])
		fprintf(b, "L_%04x:\n", s->codeEnd);
	free(marks);
}

static void WriteListing(FILE* out, const Scn16Scene_t* s, const char* name)
{
	uint32_t i;
	fprintf(out, "; %s - 16-bit scene, %u instructions, code 0x%x bytes, texts from 0x%x\n", name ? name : "scene", s->count,
		s->codeEnd, s->textStart);
	for(i = 0; i < s->count; i++)
	{
		const Scn16Insn_t* in = &s->insns[i];
		int k;
		fprintf(out, "%06x: %04x %-20s", in->off, in->op, in->def->name);
		for(k = 0; k < in->argc; k++)
		{
			const Scn16Arg_t* a = &in->args[k];
			fputc(' ', out);
			if(a->kind == 'n')
				fprintf(out, "%u:", a->num);
			else if(a->kind == 's' || a->kind == 't')
				PutArg(out, s, a);
			else
				fprintf(out, "0x%x", a->num);
		}
		fputc('\n', out);
	}
}

int Scn16_Decompile(const uint8_t* data, uint32_t size, const ScnDecOptions_t* opt, FILE* out)
{
	Scn16Scene_t s;
	char err[128];
	int problems = 0;
	FILE* mem;
	long len;
	char* text;
	if(!Scn16_Parse(data, size, &s, err, sizeof err))
	{
		fprintf(stderr, "%s: %s\n", opt->name ? opt->name : "scene", err);
		return -1;
	}
	if(opt->rawTokens)
	{
		WriteListing(out, &s, opt->name);
		Scn16_FreeScene(&s);
		return 0;
	}
	mem = opt->noVerify ? NULL : tmpfile();
	if(!mem)
	{
		WriteSource(out, &s, opt->name);
		Scn16_FreeScene(&s);
		return 0;
	}
	// write to a temporary stream, compile that back and compare, then copy it out
	WriteSource(mem, &s, opt->name);
	len = ftell(mem);
	fseek(mem, 0, SEEK_SET);
	text = (char*)malloc((size_t)len + 1);
	if(fread(text, 1, (size_t)len, mem) != (size_t)len)
		len = 0;
	text[len] = 0;
	fclose(mem);
	{
		uint8_t* back;
		uint32_t backSize;
		FILE* log = opt->verbose ? stderr : tmpfile();
		int errors = Scn16_Compile(text, (size_t)len, opt->name ? opt->name : "scene", NULL, log ? log : stderr, &back, &backSize);
		if(log && log != stderr)
			fclose(log);
		if(errors || backSize != size || memcmp(back, data, size) != 0)
		{
			problems = 1;
			if(opt->verbose)
				fprintf(stderr, "%s: the source does not reproduce the file (%s)\n", opt->name ? opt->name : "scene",
					errors ? "it does not compile" : "the bytes differ");
			fputs("// NOTE: the source below does not reproduce the file exactly\n", out);
		}
		free(back);
	}
	fwrite(text, 1, (size_t)len, out);
	free(text);
	Scn16_FreeScene(&s);
	return problems;
}

// ---- the compiler ---------------------------------------------------------------------------------------

enum TokKind
{
	T_EOF,
	T_IDENT,
	T_NUM,
	T_STR,
	T_PUNCT,
	T_DIRECTIVE
};

typedef struct Token
{
	int kind;
	int line;
	char text[SCN_NAME_MAX];
	int64_t num;
	uint8_t* bytes; // a string: Shift-JIS, no NUL (freed by the parser)
	uint32_t len;
} Token_t;

typedef struct Fixup
{
	uint32_t at;              // the dword in the code
	uint32_t text;            // the text it refers to (texts) ...
	char label[SCN_NAME_MAX]; // ... or the label (labels)
	int line;
} Fixup_t;

typedef struct Label
{
	char name[SCN_NAME_MAX];
	uint32_t off;
} Label_t;

typedef struct Comp
{
	const char* p;
	const char* end;
	int line;
	int errors;
	FILE* log;
	const PrepText_t* prep; // the expanded text being compiled: where its lines came from
	Token_t tok;
	int peeked;
	Token_t peek;
	uint8_t* code;
	uint32_t codeLen, codeCap;
	uint8_t* texts; // the text table: the strings with their NULs, in order
	uint32_t textsLen, textsCap;
	Fixup_t* fixups;
	int fixupCount, fixupCap;
	Fixup_t* textRefs;
	int textRefCount, textRefCap;
	Label_t* labels;
	int labelCount, labelCap;
} Comp_t;

static void Errorf(Comp_t* c, int line, const char* fmt, ...)
{
	va_list ap;
	c->errors++;
	if(c->errors > 50)
		return;
	{ // `line` is a line of the expanded text
		int srcLine;
		const char* file = Prep_Where(c->prep, line, &srcLine);
		fprintf(c->log, "%s:%d: error: ", file, srcLine);
	}
	va_start(ap, fmt);
	vfprintf(c->log, fmt, ap);
	va_end(ap);
	fputc('\n', c->log);
}

static int IsIdentStart(int ch)
{
	return isalpha(ch) || ch == '_' || ch >= 0x80;
}

static int IsIdentChar(int ch)
{
	return isalnum(ch) || ch == '_' || ch >= 0x80;
}

static void SkipSpace(Comp_t* c)
{
	for(;;)
	{
		while(c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\r'))
			c->p++;
		if(c->p < c->end && *c->p == '\n')
		{
			c->p++;
			c->line++;
			continue;
		}
		if(c->p + 1 < c->end && c->p[0] == '/' && c->p[1] == '/')
		{
			while(c->p < c->end && *c->p != '\n')
				c->p++;
			continue;
		}
		if(c->p + 1 < c->end && c->p[0] == '/' && c->p[1] == '*')
		{
			c->p += 2;
			while(c->p + 1 < c->end && !(c->p[0] == '*' && c->p[1] == '/'))
			{
				if(*c->p == '\n')
					c->line++;
				c->p++;
			}
			c->p += 2;
			continue;
		}
		return;
	}
}

static const char* const kPuncts[] = {"+=", "-=", "*=", "/=", "%=", "==", "!=", "<=", ">=", "(", ")", ",", ";", ":", "=",
	"<", ">", "!", "-", NULL};

static void ReadToken(Comp_t* c, Token_t* t)
{
	SkipSpace(c);
	memset(t, 0, sizeof *t);
	t->line = c->line;
	if(c->p >= c->end)
	{
		t->kind = T_EOF;
		return;
	}
	if(*c->p == '#')
	{
		const char* s = ++c->p;
		while(c->p < c->end && IsIdentChar((unsigned char)*c->p))
			c->p++;
		t->kind = T_DIRECTIVE;
		snprintf(t->text, sizeof t->text, "%.*s", (int)(c->p - s), s);
		if(strcmp(t->text, "encoding") == 0)
		{ // "#encoding utf-8": takes effect here, before the next literal is read
			int ok;
			c->p = Asm_EncodingDirective(c->p, c->end, &ok);
			if(!ok)
				Errorf(c, c->line, "#encoding utf-8 or #encoding sjis");
		}
		return;
	}
	if(*c->p == '"')
	{
		const char* next;
		const char* q;
		const char* e = Asm_ReadStringLiteral(c->p, c->end, &t->bytes, &t->len, &next);
		if(e)
		{
			Errorf(c, c->line, "%s", e);
			t->kind = T_EOF;
			c->p = c->end;
			return;
		}
		for(q = c->p; q < next; q++)
			if(*q == '\n')
				c->line++;
		c->p = next;
		t->kind = T_STR;
		return;
	}
	if(isdigit((unsigned char)*c->p))
	{
		char* e;
		t->kind = T_NUM;
		if(c->p[0] == '0' && (c->p[1] == 'x' || c->p[1] == 'X'))
			t->num = (int64_t)strtoull(c->p, &e, 16);
		else
			t->num = (int64_t)strtoull(c->p, &e, 10);
		c->p = e;
		return;
	}
	if(IsIdentStart((unsigned char)*c->p))
	{
		const char* s = c->p;
		while(c->p < c->end && IsIdentChar((unsigned char)*c->p))
			c->p++;
		t->kind = T_IDENT;
		snprintf(t->text, sizeof t->text, "%.*s", (int)(c->p - s), s);
		return;
	}
	{
		int i;
		for(i = 0; kPuncts[i]; i++)
		{
			size_t n = strlen(kPuncts[i]);
			if((size_t)(c->end - c->p) >= n && memcmp(c->p, kPuncts[i], n) == 0)
			{
				t->kind = T_PUNCT;
				strcpy(t->text, kPuncts[i]);
				c->p += n;
				return;
			}
		}
	}
	Errorf(c, c->line, "unexpected character '%c'", *c->p);
	c->p++;
	ReadToken(c, t);
}

static void Next(Comp_t* c)
{
	free(c->tok.bytes);
	if(c->peeked)
	{
		c->tok = c->peek;
		c->peeked = 0;
	}
	else
		ReadToken(c, &c->tok);
}

static const Token_t* Peek(Comp_t* c)
{
	if(!c->peeked)
	{
		ReadToken(c, &c->peek);
		c->peeked = 1;
	}
	return &c->peek;
}

static int IsPunct(const Comp_t* c, const char* p)
{
	return c->tok.kind == T_PUNCT && strcmp(c->tok.text, p) == 0;
}

static int IsIdent(const Comp_t* c, const char* p)
{
	return c->tok.kind == T_IDENT && strcmp(c->tok.text, p) == 0;
}

static int Expect(Comp_t* c, const char* p)
{
	if(!IsPunct(c, p))
	{
		Errorf(c, c->tok.line, "'%s' expected", p);
		return 0;
	}
	Next(c);
	return 1;
}

static void Emit8(Comp_t* c, uint8_t v)
{
	if(c->codeLen == c->codeCap)
	{
		c->codeCap = c->codeCap ? c->codeCap * 2 : 4096;
		c->code = (uint8_t*)realloc(c->code, c->codeCap);
	}
	c->code[c->codeLen++] = v;
}

static void Emit16(Comp_t* c, uint32_t v)
{
	Emit8(c, (uint8_t)v);
	Emit8(c, (uint8_t)(v >> 8));
}

static void Emit32(Comp_t* c, uint32_t v)
{
	Emit16(c, v & 0xffff);
	Emit16(c, v >> 16);
}

static void EmitString(Comp_t* c, const uint8_t* s, uint32_t n)
{
	uint32_t i;
	for(i = 0; i < n; i++)
		Emit8(c, s[i]);
	Emit8(c, 0);
}

static void AddFixup(Comp_t* c, const char* label, int line)
{
	if(c->fixupCount == c->fixupCap)
	{
		c->fixupCap = c->fixupCap ? c->fixupCap * 2 : 64;
		c->fixups = (Fixup_t*)realloc(c->fixups, c->fixupCap * sizeof *c->fixups);
	}
	c->fixups[c->fixupCount].at = c->codeLen;
	snprintf(c->fixups[c->fixupCount].label, SCN_NAME_MAX, "%s", label);
	c->fixups[c->fixupCount].line = line;
	c->fixupCount++;
	Emit32(c, 0);
}

static void AddText(Comp_t* c, const uint8_t* s, uint32_t n)
{
	if(c->textRefCount == c->textRefCap)
	{
		c->textRefCap = c->textRefCap ? c->textRefCap * 2 : 64;
		c->textRefs = (Fixup_t*)realloc(c->textRefs, c->textRefCap * sizeof *c->textRefs);
	}
	c->textRefs[c->textRefCount].at = c->codeLen;
	c->textRefs[c->textRefCount].text = c->textsLen;
	c->textRefCount++;
	Emit32(c, 0);
	while(c->textsLen + n + 1 > c->textsCap)
	{
		c->textsCap = (c->textsCap + n + 1024) * 2;
		c->texts = (uint8_t*)realloc(c->texts, c->textsCap);
	}
	memcpy(c->texts + c->textsLen, s, n);
	c->textsLen += n;
	c->texts[c->textsLen++] = 0;
}

static void DefineLabel(Comp_t* c, const char* name, int line)
{
	int i;
	for(i = 0; i < c->labelCount; i++)
		if(strcmp(c->labels[i].name, name) == 0)
		{
			Errorf(c, line, "label '%s' defined twice", name);
			return;
		}
	if(c->labelCount == c->labelCap)
	{
		c->labelCap = c->labelCap ? c->labelCap * 2 : 64;
		c->labels = (Label_t*)realloc(c->labels, c->labelCap * sizeof *c->labels);
	}
	snprintf(c->labels[c->labelCount].name, SCN_NAME_MAX, "%s", name);
	c->labels[c->labelCount].off = c->codeLen;
	c->labelCount++;
}

// a number, possibly negative (the dword is its two's complement)
static int ParseNumber(Comp_t* c, uint32_t* v)
{
	int neg = 0;
	if(IsPunct(c, "-"))
	{
		neg = 1;
		Next(c);
	}
	if(c->tok.kind != T_NUM)
	{
		Errorf(c, c->tok.line, "a number expected");
		return 0;
	}
	*v = (uint32_t)c->tok.num;
	if(neg)
		*v = (uint32_t)(0u - *v);
	Next(c);
	return 1;
}

// `lwork(n)`, `gwork(n)` or a number (which the engine reads as a literal, or as nothing for a destination)
static int ParseVar(Comp_t* c, uint32_t* v)
{
	if(IsIdent(c, "lwork") || IsIdent(c, "gwork"))
	{
		uint32_t hi = IsIdent(c, "lwork") ? 1 : 2, n;
		Next(c);
		if(!Expect(c, "("))
			return 0;
		if(!ParseNumber(c, &n))
			return 0;
		if(n > 0xffff)
		{
			Errorf(c, c->tok.line, "a variable index above 65535");
			return 0;
		}
		*v = hi << 16 | n;
		return Expect(c, ")");
	}
	return ParseNumber(c, v);
}

// one operand of the kind `k`
static int ParseArg(Comp_t* c, char k)
{
	uint32_t v;
	switch(k)
	{
		case 's':
		case 't':
			if(c->tok.kind != T_STR)
			{
				Errorf(c, c->tok.line, "a string expected");
				return 0;
			}
			if(k == 's')
				EmitString(c, c->tok.bytes, c->tok.len);
			else
				AddText(c, c->tok.bytes, c->tok.len);
			Next(c);
			return 1;
		case 'l':
			if(c->tok.kind == T_IDENT)
			{
				AddFixup(c, c->tok.text, c->tok.line);
				Next(c);
				return 1;
			}
			if(!ParseNumber(c, &v))
				return 0;
			Emit32(c, v);
			return 1;
		case 'v':
		case 'x':
			if(!ParseVar(c, &v))
				return 0;
			Emit32(c, v);
			return 1;
		default:
			if(!ParseNumber(c, &v))
				return 0;
			Emit32(c, v);
			return 1;
	}
}

// `name(args);` for any opcode, the arguments by its signature
static int ParseCommand(Comp_t* c, const Scn16Op_t* op)
{
	const char* sig = op->sig;
	Next(c);
	if(!Expect(c, "("))
		return 0;
	for(; *sig; sig++)
	{
		char k = *sig;
		if(k == 'D' || k == 'S' || k == 'L')
		{ // the rest of the list, counted
			uint32_t countAt = c->codeLen, n = 0;
			Emit32(c, 0);
			if(k == 'D')
				k = 'd';
			else if(k == 'S')
				k = 's';
			else
				k = 'l';
			while(!IsPunct(c, ")"))
			{
				if(n && !Expect(c, ","))
					return 0;
				if(!ParseArg(c, k))
					return 0;
				n++;
			}
			if(n == 0)
			{
				Errorf(c, c->tok.line, "%s takes at least one item in its list", op->name);
				return 0;
			}
			c->code[countAt] = (uint8_t)n;
			c->code[countAt + 1] = (uint8_t)(n >> 8);
			c->code[countAt + 2] = (uint8_t)(n >> 16);
			c->code[countAt + 3] = (uint8_t)(n >> 24);
			break;
		}
		if(sig != op->sig && !Expect(c, ","))
			return 0;
		if(!ParseArg(c, k))
			return 0;
	}
	return Expect(c, ")") && Expect(c, ";");
}

static int ParseStatement(Comp_t* c)
{
	const Scn16Op_t* op;
	int line = c->tok.line;
	if(c->tok.kind == T_IDENT && Peek(c)->kind == T_PUNCT && strcmp(Peek(c)->text, ":") == 0)
	{
		DefineLabel(c, c->tok.text, line);
		Next(c);
		Next(c);
		return 1;
	}
	if(IsIdent(c, "goto"))
	{
		Next(c);
		Emit16(c, OP_JUMP);
		return ParseArg(c, 'l') && Expect(c, ";");
	}
	if(IsIdent(c, "if"))
	{
		int neg = 0, k;
		uint32_t v;
		Next(c);
		if(!Expect(c, "("))
			return 0;
		if(IsPunct(c, "!"))
		{
			neg = 1;
			Next(c);
		}
		if(IsIdent(c, "flag"))
		{
			Next(c);
			if(!Expect(c, "(") || !ParseNumber(c, &v) || !Expect(c, ")"))
				return 0;
			Emit16(c, neg ? OP_JUMP_NOT : OP_JUMP_FLAG);
			Emit32(c, v);
		}
		else
		{
			if(neg)
			{
				Errorf(c, line, "'!' applies to flag() only");
				return 0;
			}
			if(!ParseVar(c, &v))
				return 0;
			for(k = 0; k < 6; k++)
				if(IsPunct(c, kCompareOps[k]))
					break;
			if(k == 6)
			{
				Errorf(c, c->tok.line, "a comparison expected");
				return 0;
			}
			Next(c);
			Emit16(c, OP_JUMP_EQ + (uint32_t)k);
			Emit32(c, v);
			if(!ParseArg(c, 'x'))
				return 0;
		}
		if(!Expect(c, ")"))
			return 0;
		if(!IsIdent(c, "goto"))
		{
			Errorf(c, c->tok.line, "'goto' expected");
			return 0;
		}
		Next(c);
		return ParseArg(c, 'l') && Expect(c, ";");
	}
	if(IsIdent(c, "call"))
	{
		Next(c);
		if(c->tok.kind == T_STR)
		{
			Emit16(c, OP_CALL_SCN);
			return ParseArg(c, 's') && Expect(c, ";");
		}
		Emit16(c, OP_GOSUB);
		return ParseArg(c, 'l') && Expect(c, ";");
	}
	if(IsIdent(c, "jump"))
	{
		Next(c);
		Emit16(c, OP_JUMP_SCN);
		return ParseArg(c, 's') && Expect(c, ";");
	}
	if(IsIdent(c, "return") || IsIdent(c, "end"))
	{
		Emit16(c, IsIdent(c, "end") ? OP_END : OP_RETURN);
		Next(c);
		return Expect(c, ";");
	}
	if((IsIdent(c, "lwork") || IsIdent(c, "gwork")) && Peek(c)->kind == T_PUNCT && strcmp(Peek(c)->text, "(") == 0)
	{ // an assignment
		uint32_t v;
		int k;
		if(!ParseVar(c, &v))
			return 0;
		for(k = 0; k < 6; k++)
			if(IsPunct(c, kAssignOps[k]))
				break;
		if(k == 6)
		{
			Errorf(c, c->tok.line, "an assignment expected");
			return 0;
		}
		Next(c);
		Emit16(c, OP_SET + (uint32_t)k);
		Emit32(c, v);
		return ParseArg(c, 'x') && Expect(c, ";");
	}
	if(c->tok.kind != T_IDENT)
	{
		Errorf(c, line, "a statement expected");
		return 0;
	}
	op = Scn16_FindOpByName(c->tok.text, strlen(c->tok.text));
	if(!op && strncmp(c->tok.text, "cmd_0x", 6) == 0)
	{
		op = Scn16_FindOp((uint32_t)strtoul(c->tok.text + 6, NULL, 16));
		if(!op)
		{
			Errorf(c, line, "the opcode %s is not known", c->tok.text);
			return 0;
		}
	}
	if(!op)
	{
		Errorf(c, line, "unknown command '%s'", c->tok.text);
		return 0;
	}
	Emit16(c, op->op);
	return ParseCommand(c, op);
}

int Scn16_IsSource(const char* text, size_t len)
{
	const char* p = text;
	const char* end = text + len;
	if(len >= 3 && memcmp(p, "\xEF\xBB\xBF", 3) == 0)
		p += 3; // a byte order mark
	// skip blank lines and comments up to the first directive
	while(p < end)
	{
		while(p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
			p++;
		if(p + 1 < end && p[0] == '/' && p[1] == '/')
		{
			while(p < end && *p != '\n')
				p++;
			continue;
		}
		if(p + 1 < end && p[0] == '/' && p[1] == '*')
		{
			p += 2;
			while(p + 1 < end && !(p[0] == '*' && p[1] == '/'))
				p++;
			p += 2;
			continue;
		}
		break;
	}
	if((size_t)(end - p) < 13 || memcmp(p, "#style", 6) != 0)
		return 0;
	p += 6;
	while(p < end && (*p == ' ' || *p == '\t'))
		p++;
	return (size_t)(end - p) >= 6 && memcmp(p, "inline", 6) == 0 && (end - p == 6 || !IsIdentChar((unsigned char)p[6]));
}

int Scn16_Compile(const char* text, size_t len, const char* fileName, const PrepIncludes_t* inc, FILE* log, uint8_t** outData,
	uint32_t* outSize)
{
	Comp_t c;
	PrepText_t pre;
	uint32_t textStart, size;
	int k;
	uint8_t* out;
	*outData = NULL;
	*outSize = 0;
	// the includes and the defined names first (prep.h): the scene is compiled from the expanded text
	Prep_Expand(text, len, fileName ? fileName : "source", inc, log, &pre);
	if(pre.errors)
	{
		k = pre.errors;
		Prep_Free(&pre);
		return k;
	}
	memset(&c, 0, sizeof c);
	c.p = pre.text;
	c.end = pre.text + pre.len;
	c.line = 1;
	c.log = log ? log : stderr;
	c.prep = &pre;
	Next(&c);
	if(c.tok.kind != T_DIRECTIVE || strcmp(c.tok.text, "style") != 0 || Peek(&c)->kind != T_IDENT ||
		strcmp(Peek(&c)->text, "inline") != 0)
		Errorf(&c, c.tok.line, "'#style inline' expected first");
	else
	{
		Next(&c);
		Next(&c);
	}
	while(c.tok.kind != T_EOF && c.errors <= 50)
	{
		if(c.tok.kind == T_DIRECTIVE)
		{
			if(strcmp(c.tok.text, "encoding") != 0) // (the lexer has switched the encoding of the literals)
				Errorf(&c, c.tok.line, "#%s: a 16-bit scene knows '#style inline', '#encoding', '#include', '#define' and '#undef'", c.tok.text);
			Next(&c);
			continue;
		}
		if(!ParseStatement(&c))
		{ // resync at the next ';'
			while(c.tok.kind != T_EOF && !IsPunct(&c, ";"))
				Next(&c);
			if(IsPunct(&c, ";"))
				Next(&c);
		}
	}
	free(c.tok.bytes);
	c.tok.bytes = NULL;
	if(c.peeked)
		free(c.peek.bytes);
	// the layout: the code, zero padding to a multiple of 16 when there are texts, the texts
	textStart = c.textsLen ? (c.codeLen + 15) & ~15u : c.codeLen;
	size = textStart + c.textsLen;
	out = (uint8_t*)calloc(size + 1, 1);
	if(c.codeLen)
		memcpy(out, c.code, c.codeLen);
	if(c.textsLen) // a scene without a message has no text table
		memcpy(out + textStart, c.texts, c.textsLen);
	for(k = 0; k < c.textRefCount; k++)
	{
		uint32_t v = textStart + c.textRefs[k].text;
		out[c.textRefs[k].at] = (uint8_t)v;
		out[c.textRefs[k].at + 1] = (uint8_t)(v >> 8);
		out[c.textRefs[k].at + 2] = (uint8_t)(v >> 16);
		out[c.textRefs[k].at + 3] = (uint8_t)(v >> 24);
	}
	for(k = 0; k < c.fixupCount; k++)
	{
		int j;
		for(j = 0; j < c.labelCount; j++)
			if(strcmp(c.labels[j].name, c.fixups[k].label) == 0)
				break;
		if(j == c.labelCount)
			Errorf(&c, c.fixups[k].line, "label '%s' is not defined", c.fixups[k].label);
		else
		{
			uint32_t v = c.labels[j].off;
			out[c.fixups[k].at] = (uint8_t)v;
			out[c.fixups[k].at + 1] = (uint8_t)(v >> 8);
			out[c.fixups[k].at + 2] = (uint8_t)(v >> 16);
			out[c.fixups[k].at + 3] = (uint8_t)(v >> 24);
		}
	}
	free(c.code);
	free(c.texts);
	free(c.fixups);
	free(c.textRefs);
	free(c.labels);
	Prep_Free(&pre);
	if(c.errors)
	{
		free(out);
		return c.errors;
	}
	*outData = out;
	*outSize = size;
	return 0;
}
