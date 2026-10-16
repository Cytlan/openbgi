/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * decode.c - one instruction: operand formats, decoding, mnemonics, text;
 *            interface in asm.h
 *
 * The operand formats of the base instructions are those of src/vm/
 * ops_base.c (RdU8 / RdU16 / RdU32 after the opcode); the 1.667+
 * superinstructions follow docs/versions.md.  The family instructions
 * (0x80 .. 0xFF) take nothing from the code stream but their second byte.
 *
 * The mnemonic lookups (Asm_OpName, Asm_LookupName) work on the generated
 * table kAsmOpNames (opnames.c); which opcodes a generation defines comes
 * from kOpSet (version.h).  Asm_Format writes an instruction so that the
 * assembler reads it back to the same bytes under the same generation.
 */
#include "bgi/asm.h"

// ---- families ----------------------------------------------------------------------------

// a family dispatcher byte with its mnemonic prefix and its opcode set
typedef struct FamName
{
	int byte;         // the first byte of the instruction (0x80 for "sys")
	const char* name; // the prefix in the listing ("sys" in "sys.yield")
	OpFamily_t fam;   // the index into kOpSet; OPFAM_COUNT when the family has no set
} FamName_t;

// every family byte the listing knows; "user" (0xFF) is the script-defined instructions
static const FamName_t kFamilies[] = {
	{0x7f, "sys7f", OPFAM_7F},
	{0x80, "sys", OPFAM_80},
	{0x81, "sys81", OPFAM_81},
	{0x90, "gfx0", OPFAM_90},
	{0x91, "gfx1", OPFAM_91},
	{0x92, "gfx2", OPFAM_92},
	{0xa0, "snd", OPFAM_A0},
	{0xb0, "ext0", OPFAM_B0},
	{0xc0, "ext1", OPFAM_C0},
	{0xd0, "eval", OPFAM_D0},
	{0xe0, "ext2", OPFAM_E0},
	{0xef, "obgi", OPFAM_EF}, // the OpenBGI extensions
	{0xff, "user", OPFAM_COUNT},
};

// the mnemonic prefix of a family byte ("sys" for 0x80); NULL when the byte is not a family
const char* Asm_FamilyName(int fam)
{
	size_t i;
	for(i = 0; i < BGI_COUNTOF(kFamilies); i++)
		if(kFamilies[i].byte == fam)
			return kFamilies[i].name;
	return NULL;
}

// the family byte of a prefix (`len` bytes of `name`); -1 when it is not one
int Asm_FamilyByte(const char* name, size_t len)
{
	size_t i;
	for(i = 0; i < BGI_COUNTOF(kFamilies); i++)
		if(strlen(kFamilies[i].name) == len && memcmp(kFamilies[i].name, name, len) == 0)
			return kFamilies[i].byte;
	return -1;
}

// the opcode-set family of a family byte (OPFAM_COUNT for the user instructions, which have no set)
static OpFamily_t FamilyOfByte(int fam)
{
	size_t i;
	if(fam == 0)
		return OPFAM_MAIN;
	for(i = 0; i < BGI_COUNTOF(kFamilies); i++)
		if(kFamilies[i].byte == fam)
			return kFamilies[i].fam;
	return OPFAM_COUNT;
}

/* 1 when the generation defines the opcode: a base instruction (fam 0) in
 * the main table, a family instruction in its family's set.  A user
 * instruction ("FF xx") is always defined, since the script fills those
 * slots itself.  gen GEN_COUNT accepts every opcode any build has. */
static int Defined(int fam, int op, EngineGen_t gen)
{
	OpFamily_t f = FamilyOfByte(fam);
	if(fam == 0xff)
		return 1; // user instructions: any slot
	if(f == OPFAM_COUNT)
		return 0;
	if(gen == GEN_COUNT)
		return kOpSet[f][op] != 0; // defined in some build
	return OpSet_Has(f, op, gen);
}

/* the mnemonic of an opcode in a generation, NULL when it has none; under
 * GEN_COUNT a reused number gets the name still valid in the newest build,
 * else the first name the table lists for it */
const char* Asm_OpName(int fam, int op, EngineGen_t gen)
{
	size_t i;
	const char* any = NULL;
	for(i = 0; i < kAsmOpNameCount; i++)
	{
		const AsmOpName_t* n = &kAsmOpNames[i];
		if(n->fam != fam || n->op != op)
			continue;
		if(gen == GEN_COUNT)
		{
			if(!any || n->to == GEN_LAST)
				any = n->name; // the newest meaning
			continue;
		}
		if(gen >= n->from && gen <= n->to)
			return n->name;
	}
	return any;
}

/* The opcode of a mnemonic: an optional family prefix and a dot, then a
 * name or a hexadecimal number ("push_i8", "sys.yield", "sys.0x5f",
 * "0x3c").  An unknown prefix fails; a bare number is a base opcode and
 * must be below 0x80 (the family dispatchers are not instructions of their
 * own).  A name is looked up in `gen`, the newest meaning winning when
 * several entries match; a name no entry of `gen` has is looked up again
 * in every generation so that a listing of another build still assembles
 * (the assembler warns about the opcode afterwards).  1 with `*fam` (0 for
 * a base instruction) and `*op` set, 0 when unknown. */
int Asm_LookupName(const char* name, size_t len, EngineGen_t gen, int* fam, int* op)
{
	const char* dot = memchr(name, '.', len);
	size_t i;
	int f = 0;
	const char* rest = name;
	size_t restLen = len;
	if(dot)
	{
		f = Asm_FamilyByte(name, (size_t)(dot - name));
		if(f < 0)
			return 0;
		rest = dot + 1;
		restLen = len - (size_t)(dot - name) - 1;
	}
	// a number: "sys.0x5f", "0x3c"
	if(restLen > 2 && rest[0] == '0' && (rest[1] == 'x' || rest[1] == 'X'))
	{
		char* end;
		long v = strtol(rest + 2, &end, 16);
		if(end == rest + restLen && v >= 0 && v < 256 && (dot || v < 0x80))
		{
			*fam = f;
			*op = (int)v;
			return 1;
		}
	}
	{
		const AsmOpName_t* best = NULL;
		for(i = 0; i < kAsmOpNameCount; i++)
		{
			const AsmOpName_t* n = &kAsmOpNames[i];
			if(n->fam != f || strlen(n->name) != restLen || memcmp(n->name, rest, restLen) != 0)
				continue;
			if(gen != GEN_COUNT && (gen < n->from || gen > n->to))
				continue;
			// "any": the newest meaning of the name, as Asm_OpName gives the newest name of a number
			if(!best || n->to > best->to || (n->to == best->to && n->from > best->from))
				best = n;
		}
		if(best)
		{
			*fam = f;
			*op = best->op;
			return 1;
		}
	}
	// a name of another generation still resolves (with the assembler's warning for the opcode)
	if(gen != GEN_COUNT)
		return Asm_LookupName(name, len, GEN_COUNT, fam, op);
	return 0;
}

// ---- operand formats ------------------------------------------------------------------------

/* The operand kinds of a base opcode as a string of format letters, one
 * per operand, in code order (KindOfCode maps a letter to its kind; as.c
 * has the same table for encoding, and the round-trip test keeps the two
 * in step).  An opcode without inline operands gives "".  The conditional
 * jump "15" of 1.667+ has a second operand only when the condition has
 * bit 3: the '?' stands for that optional rel16. */
static const char* Formats(int op, EngineGen_t gen)
{
	switch(op)
	{
		case 0x00: return "b";
		case 0x01: return "h";
		case 0x02: return "w";
		case 0x04: return "n";
		case 0x05: return "c";
		case 0x06: return "j";
		case 0x08:
		case 0x09:
		case 0x0a: return "z";
		case 0x0b: return "i";
		case 0x0c: return "zq";
		case 0x15: return (gen == GEN_COUNT || gen >= GEN_1_667) ? "k?" : "k";
		case 0x0f:
		case 0x19:
		case 0x1d:
		case 0x1e: return "L";
		case 0x12: return "S";
		case 0x18: return "w";
		case 0x1a:
		case 0x1b: return "Ls";
		case 0x1c: return "Lb";
		case 0x1f:
		case 0x2c:
		case 0x2d:
		case 0x2e:
		case 0x2f: return "s";
		case 0x3b: return "kr";
		case 0x3c: return "b";
		default: return "";
	}
}

// the operand kind of a format letter; ASM_OPD_NONE for a letter that is not one
static enum AsmOperandKind KindOfCode(char c)
{
	switch(c)
	{
		case 'b': return ASM_OPD_IMM8;
		case 'h': return ASM_OPD_IMM16;
		case 'w': return ASM_OPD_IMM32;
		case 'n': return ASM_OPD_LOCAL;
		case 'c': return ASM_OPD_DATAREL;
		case 'j': return ASM_OPD_CODEREL;
		case 'z': return ASM_OPD_SIZE;
		case 'i': return ASM_OPD_BYTES;
		case 'q': return ASM_OPD_COUNT;
		case 'k': return ASM_OPD_COND;
		case 'L': return ASM_OPD_LOCAL16;
		case 's': return ASM_OPD_SLEB;
		case 'S': return ASM_OPD_SLEBREL;
		case 'r': return ASM_OPD_REL16;
		default: return ASM_OPD_NONE;
	}
}

/* read a signed LEB128 varint (7 bits per byte, bit 7 continues, bit 6 of
 * the last byte is the sign) from `p`: 1 with the value and the number of
 * bytes consumed; 0 when no final byte comes within `avail` bytes (or
 * within the 10 bytes a 64-bit value can take) */
static int ReadSleb(const uint8_t* p, uint32_t avail, int64_t* out, uint32_t* len)
{
	int64_t v = 0;
	int shift = 0;
	uint32_t i;
	for(i = 0; i < avail && i < 10; i++)
	{
		uint8_t b = p[i];
		v |= (int64_t)(b & 0x7f) << shift;
		shift += 7;
		if(!(b & 0x80))
		{
			if((b & 0x40) && shift < 64)
				v |= -((int64_t)1 << shift);
			*out = v;
			*len = i + 1;
			return 1;
		}
	}
	return 0;
}

/* Decode the instruction at `off` into `*out`: the opcode (two bytes for a
 * family instruction), then the inline operands per Formats, with the
 * target of a relative operand resolved to an area offset.  1 on success;
 * 0 when the opcode is undefined in `gen`, a family instruction has no
 * second byte, or an operand runs past `size`. */
int Asm_Decode(const uint8_t* area, uint32_t size, uint32_t off, EngineGen_t gen, AsmInsn_t* out)
{
	const char* fmt;
	uint32_t p = off;
	int i;
	memset(out, 0, sizeof *out);
	if(off >= size)
		return 0;
	out->off = off;
	out->op = area[p++];
	// 7F is the nop of the builds up to 1.662 and a family dispatcher from 1.667 on;
	// under "any" it is read as the one-byte instruction
	if(out->op >= 0x80 ? Asm_FamilyName(out->op) != NULL : (out->op == 0x7f && gen != GEN_COUNT && gen >= GEN_1_667))
	{
		out->fam = out->op;
		if(p >= size)
			return 0;
		out->op = area[p++];
		if(!Defined(out->fam, out->op, gen))
		{
			if(gen == GEN_COUNT || !Defined(out->fam, out->op, GEN_COUNT))
				return 0;
			out->foreign = 1; // some other build's instruction, same shape
		}
		out->len = p - off;
		return 1;
	}
	if(out->op >= 0x80 || !Defined(0, out->op, gen))
		return 0; // an unknown family byte, or a base opcode the generation lacks
	fmt = Formats(out->op, gen);
	for(i = 0; fmt[i]; i++)
	{
		enum AsmOperandKind k;
		uint32_t need = 0, n;
		int64_t v = 0;
		char c = fmt[i];
		if(c == '?')
		{ // "15" of 1.667+: a relative target follows when the condition has bit 3
			if(!(out->v[0] & 8))
				break;
			c = 'r';
		}
		k = KindOfCode(c);
		// the fixed-size operands are bounds-checked before they are read
		switch(k)
		{
			case ASM_OPD_IMM8:
			case ASM_OPD_SIZE:
			case ASM_OPD_COUNT:
			case ASM_OPD_COND: need = 1; break;
			case ASM_OPD_IMM16:
			case ASM_OPD_LOCAL:
			case ASM_OPD_DATAREL:
			case ASM_OPD_CODEREL:
			case ASM_OPD_LOCAL16:
			case ASM_OPD_REL16: need = 2; break;
			case ASM_OPD_IMM32: need = 4; break;
			case ASM_OPD_BYTES: need = 1; break; // the count byte; the bytes are checked below
			default: need = 0; break;            // the varints check their own length
		}
		if(p + need > size)
			return 0;
		switch(k)
		{
			case ASM_OPD_IMM8: v = (int8_t)area[p]; break;
			case ASM_OPD_SIZE:
			case ASM_OPD_COUNT:
			case ASM_OPD_COND: v = area[p]; break;
			case ASM_OPD_IMM16:
			case ASM_OPD_DATAREL:
			case ASM_OPD_CODEREL:
			case ASM_OPD_REL16: v = (int16_t)(area[p] | (area[p + 1] << 8)); break;
			case ASM_OPD_LOCAL:
			case ASM_OPD_LOCAL16: v = area[p] | (area[p + 1] << 8); break; // unsigned
			case ASM_OPD_IMM32: v = (uint32_t)(area[p] | (area[p + 1] << 8) | (area[p + 2] << 16) | ((uint32_t)area[p + 3] << 24)); break;
			case ASM_OPD_BYTES:
				n = area[p];
				if(p + 1 + n > size)
					return 0;
				v = n; // the value is the count; the bytes themselves stay in the area
				out->bytes = area + p + 1;
				need = 1 + n;
				break;
			case ASM_OPD_SLEB:
			case ASM_OPD_SLEBREL:
				if(!ReadSleb(area + p, size - p, &v, &n))
					return 0;
				need = n;
				break;
			default: return 0;
		}
		out->kinds[out->count] = (uint8_t)k;
		out->v[out->count] = v;
		out->count++;
		p += need;
		// the 16-bit relative operands count from the opcode's own offset, the
		// LEB128 one from the end of the instruction (which is where p is now)
		if(k == ASM_OPD_CODEREL || k == ASM_OPD_REL16)
		{
			out->target = (uint32_t)((int64_t)off + v);
			out->targetKind = 1;
		}
		else if(k == ASM_OPD_DATAREL)
		{
			out->target = (uint32_t)((int64_t)off + v);
			out->targetKind = 2;
		}
		else if(k == ASM_OPD_SLEBREL)
		{
			out->target = (uint32_t)((int64_t)p + v);
			out->targetKind = 1;
		}
	}
	out->len = p - off;
	return 1;
}

// the length in bytes of the instruction at `off`; 0 when it does not decode
uint32_t Asm_InsnLength(const uint8_t* area, uint32_t size, uint32_t off, EngineGen_t gen)
{
	AsmInsn_t in;
	return Asm_Decode(area, size, off, gen, &in) ? in.len : 0;
}

// ---- text --------------------------------------------------------------------------------------

/* the name of an opcode when the assembler would turn it back into the same
 * opcode under this generation, else NULL: under "any" the name of a
 * renumbered instruction resolves to its newest number, so an older
 * number must be written numerically to round-trip */
static const char* RoundTripName(int fam, int op, EngineGen_t gen)
{
	const char* name = Asm_OpName(fam, op, gen);
	char full[128];
	int f2, o2;
	if(!name)
		return NULL;
	if(fam)
		snprintf(full, sizeof full, "%s.%s", Asm_FamilyName(fam), name);
	else
		snprintf(full, sizeof full, "%s", name);
	if(Asm_LookupName(full, strlen(full), gen, &f2, &o2) && f2 == fam && o2 == op)
		return name;
	return NULL;
}

/* write the instruction as the assembler reads it: the mnemonic (numeric
 * when its name would not round-trip), then the operands separated by ", "
 * - signed immediates in decimal, offsets and 32-bit values in hex, a
 * local reference as "offset, size", a relative target as its resolved
 * offset, inline bytes as a list.  Truncates to `n` bytes like snprintf
 * and returns the full length. */
size_t Asm_Format(const AsmInsn_t* in, EngineGen_t gen, char* buf, size_t n)
{
	const char* name = RoundTripName(in->fam, in->op, gen);
	size_t o = 0;
	int i;
#define PUT(...)                                                    \
	do                                                              \
	{                                                               \
		int w_ = snprintf(buf + o, o < n ? n - o : 0, __VA_ARGS__); \
		if(w_ > 0)                                                  \
			o += (size_t)w_;                                        \
	} while(0)
	if(in->fam)
	{
		if(name)
			PUT("%s.%s", Asm_FamilyName(in->fam), name);
		else
			PUT("%s.0x%02x", Asm_FamilyName(in->fam), in->op);
		return o; // a family instruction has no operands
	}
	if(name)
		PUT("%s", name);
	else
		PUT("0x%02x", in->op); // a bare number is the base opcode itself
	for(i = 0; i < in->count; i++)
	{
		int64_t v = in->v[i];
		PUT(i ? ", " : " ");
		switch(in->kinds[i])
		{
			case ASM_OPD_IMM8:
			case ASM_OPD_IMM16:
			case ASM_OPD_SLEB: PUT("%lld", (long long)v); break;
			case ASM_OPD_IMM32: PUT("0x%lx", (unsigned long)(uint32_t)v); break;
			case ASM_OPD_LOCAL: PUT("0x%lx", (unsigned long)v); break;
			case ASM_OPD_LOCAL16: PUT("0x%lx, %d", (unsigned long)(v & 0x3fff), (int)(v >> 14)); break; // offset, size code
			case ASM_OPD_DATAREL:
			case ASM_OPD_CODEREL:
			case ASM_OPD_REL16:
			case ASM_OPD_SLEBREL: PUT("0x%lx", (unsigned long)in->target); break;
			case ASM_OPD_SIZE:
			case ASM_OPD_COUNT:
			case ASM_OPD_COND: PUT("%d", (int)v); break;
			case ASM_OPD_BYTES:
			{
				int k;
				for(k = 0; k < (int)v; k++)
					PUT("%s0x%02x", k ? ", " : "", in->bytes[k]);
				if(v == 0)
					o--; // no bytes: nothing after the mnemonic (the separator goes)
				break;
			}
			default: break;
		}
	}
#undef PUT
	return o;
}

int Asm_StackEffect(int fam, int op, EngineGen_t gen, int* pops, int* pushes)
{
	size_t i;
	const AsmOpStack_t* best = NULL;
	for(i = 0; i < kAsmOpStackCount; i++)
	{
		const AsmOpStack_t* e = &kAsmOpStack[i];
		if(e->fam != fam || e->op != op)
			continue;
		if(gen == GEN_COUNT)
		{ // any: the newest entry
			if(!best || e->to > best->to || (e->to == best->to && e->from > best->from))
				best = e;
		}
		else if(gen >= e->from && gen <= e->to)
			best = e;
	}
	if(!best)
		return 0;
	*pops = best->pops;
	*pushes = best->pushes;
	return 1;
}
