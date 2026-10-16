/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dis.c - a program file as an assembler listing (Dis_Program); interface
 *         in asm.h
 *
 * The code is found by following the flow: from offset 0 (where the
 * engine starts a program) and from every target of a relative branch
 * (`push_code_off`, the 1.667+ relative jumps), instruction by instruction
 * until a return, an unconditional jump, an instruction that ends the
 * thread or an undefined opcode.  Everything the flow never reaches is
 * data: the strings the code points at with `push_code_addr` (or a tagged
 * `push_i32`), and whatever else the compiler left.  A listing of a
 * program whose flow runs into an undefined opcode says so at that place;
 * the bytes after it are listed as data so that the file still
 * round-trips through the assembler.
 *
 * The work is done in three steps over two per-byte maps (`kind`: what
 * the byte is, `label`: which labels it needs): the flows are followed
 * from a work list (Follow), gaps that decode cleanly are taken as
 * unreferenced code (FindUnreferencedCode), then the area is written from
 * start to end, code with Asm_Format and labels in place of its targets,
 * data as `.str` and `.db` lines (WriteCode, WriteData).
 *
 * Labels: `sub_XXXX` for a call target, `loc_XXXX` for a jump target,
 * `str_XXXX` for a referenced string, `dat_XXXX` for other referenced
 * data.  Strings are written in UTF-8 when the conversion to and from
 * Shift-JIS is exact, else with \xNN escapes.
 */
#include "bgi/asm.h"
#include "bgi/os.h"

// the kind of a byte of the area (Dis_t.kind)
#define K_UNKNOWN 0 // not reached yet; becomes K_DATA once the analysis is done
#define K_CODE    1 // the first byte of an instruction
#define K_INSIDE  2 // a later byte of an instruction
#define K_DATA    3 // data (anything the code never reaches)

// the labels an offset needs (Dis_t.label, a set of bits; L_DEAD below)
#define L_JUMP    1 // a branch target: "loc_"
#define L_CALL    2 // a call target: "sub_" (wins over L_JUMP)
#define L_DATA    4 // referenced data: "str_" when a string lies there, else "dat_"

// the state of one disassembly
typedef struct Dis
{
	const uint8_t* area;            // the code area (the file after its 16-byte header)
	uint32_t size;                  // its size in bytes
	EngineGen_t gen;                // the generation to decode with (GEN_COUNT: any)
	const EngineProfile_t* profile; // the pointer layout for tagged values
	uint8_t* kind;                  // per byte: K_*
	uint8_t* label;                 // per byte: L_* bits
	uint32_t* work;                 // the offsets still to follow
	uint32_t workCount, workCap;    // its fill and capacity
	int problems;                   // the "; problem:" lines written so far (the result of Dis_Program)
	FILE* out;                      // the listing
	const DisOptions_t* opt;        // the caller's options
} Dis_t;

// queue an offset for Follow (one outside the area is dropped)
static void Push(Dis_t* d, uint32_t off)
{
	if(off >= d->size)
		return;
	if(d->workCount == d->workCap)
	{
		d->workCap = d->workCap ? d->workCap * 2 : 256;
		d->work = (uint32_t*)realloc(d->work, d->workCap * sizeof *d->work);
	}
	d->work[d->workCount++] = off;
}

/* the area offset a 32-bit value points at when it is a tagged pointer
 * into the code area (tag << tagShift | offset, the profile's layout) and
 * the offset is inside the area; -1 when it is not */
static int64_t CodePointer(const Dis_t* d, uint32_t v)
{
	uint32_t tag = v >> d->profile->tagShift, off = v & ((1u << d->profile->tagShift) - 1u);
	if(tag == d->profile->tagCode && off < d->size)
		return off;
	return -1;
}

/* 1 when the instruction ends its flow: "14" jmp, "17" ret, "12" jmp_rel,
 * or a system call the thread never returns from.  A conditional jump
 * falls through, so it does not. */
static int EndsFlow(const AsmInsn_t* in)
{
	if(in->fam == 0)
		return in->op == 0x14 || in->op == 0x17 || in->op == 0x12;
	if(in->fam == 0x80)
		return in->op == 0x45 || in->op == 0x6a || in->op == 0x6b; // exit_thread, quit, reboot
	return 0;
}

/* Follow one flow from `off`: mark each instruction K_CODE / K_INSIDE,
 * queue the target of every relative branch (labelled "sub_" when the
 * `push_code_off` is followed by a `call`, else "loc_") and label the data
 * that `push_code_addr`, a tagged `push_i32` or a tagged `load_abs` refer
 * to, until the flow ends or joins code already followed.  A flow that
 * runs into data, into the middle of an instruction, into an undefined
 * opcode or across another instruction stops there with a "; problem:"
 * line. */
static void Follow(Dis_t* d, uint32_t off)
{
	AsmInsn_t in;
	uint32_t i;
	while(off < d->size)
	{
		if(d->kind[off] == K_CODE)
			return; // joined a flow already followed
		if(d->kind[off] != K_UNKNOWN)
		{
			if(d->out)
				fprintf(d->out, "; problem: the flow at 0x%04x runs into the middle of an instruction or into data\n", (unsigned)off);
			d->problems++;
			return;
		}
		if(!Asm_Decode(d->area, d->size, off, d->gen, &in))
		{
			if(d->out)
				fprintf(d->out, "; problem: undefined opcode 0x%02x at 0x%04x (the bytes from there are listed as data)\n",
					d->area[off], (unsigned)off);
			d->problems++;
			return;
		}
		if(in.foreign && d->out)
			fprintf(d->out, "; note: instruction %02x %02x at 0x%04x is not defined in this build (another build's)\n", in.fam, in.op,
				(unsigned)off);
		for(i = off; i < off + in.len; i++)
			if(d->kind[i] != K_UNKNOWN)
			{
				if(d->out)
					fprintf(d->out, "; problem: the instruction at 0x%04x overlaps another\n", (unsigned)off);
				d->problems++;
				return;
			}
		d->kind[off] = K_CODE;
		for(i = off + 1; i < off + in.len; i++)
			d->kind[i] = K_INSIDE;
		if(in.targetKind == 1)
		{
			// a push_code_off ("06") followed by a call ("16") names a subroutine
			int isCall = in.fam == 0 && in.op == 0x06 && off + in.len < d->size && d->area[off + in.len] == 0x16;
			d->label[in.target] |= isCall ? L_CALL : L_JUMP;
			Push(d, in.target);
		}
		else if(in.targetKind == 2)
			d->label[in.target] |= L_DATA;
		if(in.fam == 0 && (in.op == 0x02 || in.op == 0x18))
		{ // push_i32 / load_abs: a tagged pointer into the code area refers to data
			int64_t p = CodePointer(d, (uint32_t)in.v[0]);
			if(p >= 0)
				d->label[p] |= L_DATA;
		}
		if(EndsFlow(&in))
			return;
		off += in.len;
	}
}

/* The compiler leaves code nothing refers to: functions without a caller
 * and the tail of a function after its last jump.  A gap between what the
 * flows reached is taken as code when it decodes cleanly from its start
 * to its end - every opcode defined, the last instruction ending its flow
 * (a return or a jump) exactly where the gap ends or where a string
 * begins.  Such a block is labelled and marked "unreferenced" in the
 * listing; strings and zero padding never pass the test. */
#define L_DEAD 8 // the label bit of such a block: "; unreferenced" after its label

static uint32_t StringLen(const Dis_t* d, uint32_t off, uint32_t limit);

/* 1 when the run [off, end) is clearly not code: a string of at least four
 * bytes or a shorter one with a non-ASCII byte starts there, or it is zero
 * padding up to `end` */
static int ClearlyData(const Dis_t* d, uint32_t off, uint32_t end)
{
	uint32_t n = StringLen(d, off, end), i;
	if(n >= 4)
		return 1;
	for(i = off; i < off + n; i++)
		if(d->area[i] >= 0x80)
			return 1; // a Japanese string
	for(i = off; i < end && d->area[i] == 0; i++)
		;
	return i == end;
}

/* 1 when the bytes from `off` decode as a block of code that ends its flow
 * exactly at `end` or where clear data begins, or that runs exactly up to
 * a code label at `end` (dead code falling into live code); `*reached`
 * receives where the decoding stopped.  0 when an instruction fails to
 * decode or runs past `end`, or the last one neither ends the flow nor
 * meets such a label. */
static int DecodesCleanly(Dis_t* d, uint32_t off, uint32_t end, uint32_t* reached)
{
	AsmInsn_t in;
	int ended = 0;
	uint32_t p = off;
	while(p < end)
	{
		if(!Asm_Decode(d->area, d->size, p, d->gen, &in) || p + in.len > end)
			return 0;
		ended = EndsFlow(&in);
		p += in.len;
		if(ended && (p == end || ClearlyData(d, p, end)))
			break;
	}
	*reached = p;
	return ended || (p == end && end < d->size && (d->label[end] & (L_JUMP | L_CALL)) != 0);
}

/* Scan the gaps the flows left (runs of K_UNKNOWN bytes up to the next
 * labelled offset, skipping referenced data) and follow every one that
 * decodes cleanly as a block of code.  A block starting with "10" push_fp,
 * the way a function begins, is labelled "sub_", any other "loc_".
 * Repeated until a scan finds nothing, since a block's own branches may
 * reach into gaps already passed. */
static void FindUnreferencedCode(Dis_t* d)
{
	uint32_t off = 0;
	int again;
	do
	{
		again = 0;
		off = 0;
		while(off < d->size)
		{
			uint32_t end, reached;
			if(d->kind[off] != K_UNKNOWN || (d->label[off] & L_DATA))
			{
				off++;
				continue;
			}
			end = off;
			while(end < d->size && d->kind[end] == K_UNKNOWN && (end == off || !d->label[end]))
				end++;
			if(end - off >= 2 && !ClearlyData(d, off, end) && DecodesCleanly(d, off, end, &reached))
			{
				d->label[off] |= L_DEAD | (d->area[off] == 0x10 ? L_CALL : L_JUMP);
				Push(d, off);
				while(d->workCount)
					Follow(d, d->work[--d->workCount]);
				again = 1; // the block's own branches may have reached more
			}
			off = end;
		}
	} while(again);
}

// ---- strings --------------------------------------------------------------------------------

// 1 for the first byte of a two-byte Shift-JIS character
static int SjisLead(uint8_t c)
{
	return (c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc);
}

/* the length (without the NUL) of a NUL-terminated string at `off` made of
 * printable ASCII, newline / return / tab, half-width katakana and two-byte
 * Shift-JIS characters, terminator included inside [off, limit); 0 when
 * the bytes are not such a string or the string is empty */
static uint32_t StringLen(const Dis_t* d, uint32_t off, uint32_t limit)
{
	uint32_t p = off;
	while(p < limit)
	{
		uint8_t c = d->area[p];
		if(c == 0)
			return p > off ? p - off : 0;
		if(c >= 0x20 && c < 0x7f)
			p++;
		else if(c >= 0xa1 && c <= 0xdf)
			p++;
		else if(SjisLead(c) && p + 1 < limit && d->area[p + 1] >= 0x40 && d->area[p + 1] != 0x7f && d->area[p + 1] <= 0xfc)
			p += 2;
		else if(c == '\n' || c == '\r' || c == '\t')
			p++;
		else
			return 0;
	}
	return 0; // no terminator before the limit
}

/* write the first 48 characters of `n` bytes of Shift-JIS, quoted, as the
 * comment after an instruction that refers to the string; control
 * characters become spaces and a cut is marked with "..." */
static void Preview(FILE* out, const uint8_t* s, uint32_t n)
{
	char* sjis = (char*)malloc(n + 1);
	char* utf = (char*)malloc(n * 3 + 1);
	uint32_t i, shown = 0;
	memcpy(sjis, s, n);
	sjis[n] = 0;
	OS_SjisToUtf8(sjis, utf, n * 3 + 1);
	fputc('"', out);
	for(i = 0; utf[i]; i++)
	{
		uint8_t c = (uint8_t)utf[i];
		if((c & 0xc0) != 0x80)
		{ // the start of a character: the preview stops between characters
			if(shown == 48)
				break;
			shown++;
		}
		if(c < 0x20)
			fputc(' ', out);
		else
			fputc((int)c, out);
	}
	if(utf[i])
		fputs("...", out);
	fputc('"', out);
	free(sjis);
	free(utf);
}

// ---- the listing ------------------------------------------------------------------------------

/* the label of an offset from its L_* bits: "sub_XXXX" for a call target,
 * "loc_XXXX" for a jump target, "str_XXXX" for data only referenced as
 * data when a string lies there, "dat_XXXX" for other data */
static void Label(Dis_t* d, uint32_t off, char* buf, size_t n)
{
	uint8_t l = d->label[off];
	const char* prefix = (l & L_CALL) ? "sub" : (l & L_JUMP) ? "loc"
															 : "dat";
	if(l == L_DATA && d->kind[off] == K_DATA)
	{
		uint32_t limit = off;
		while(limit < d->size && d->kind[limit] == K_DATA)
			limit++;
		if(StringLen(d, off, limit))
			prefix = "str";
	}
	snprintf(buf, n, "%s_%04x", prefix, (unsigned)off);
}

// the byte column of a line: up to six bytes in hex, ".." when there are more, padded to 20 columns
static void Bytes(Dis_t* d, uint32_t off, uint32_t len)
{
	uint32_t i;
	char col[40];
	size_t o = 0;
	col[0] = 0;
	for(i = 0; i < len && i < 6; i++)
		o += (size_t)snprintf(col + o, sizeof col - o, "%02x ", d->area[off + i]);
	if(len > 6)
		snprintf(col + o, sizeof col - o, "..");
	fprintf(d->out, "%-20s", col);
}

/* the optional offset and byte columns of a line (DisOptions showOffsets /
 * showBytes); they end with '|', which the assembler takes as the start of
 * the statement, so that such a listing assembles too */
static void LinePrefix(Dis_t* d, uint32_t off, uint32_t len)
{
	if(!d->opt || (!d->opt->showOffsets && !d->opt->showBytes))
		return;
	if(d->opt->showOffsets)
		fprintf(d->out, "%04x  ", (unsigned)off);
	if(d->opt->showBytes)
		Bytes(d, off, len);
	fputs("|", d->out);
}

/* the mnemonic of an instruction alone (Asm_Format of a copy without
 * operands), numeric when its name would not assemble back to it */
static const char* Mnemonic(const Dis_t* d, const AsmInsn_t* in, char* buf, size_t n)
{
	AsmInsn_t bare = *in;
	size_t o;
	bare.count = 0;
	bare.targetKind = 0;
	o = Asm_Format(&bare, d->gen, buf, n);
	(void)o;
	return buf;
}

/* write the line of one instruction: the mnemonic with its operands, a
 * relative target or a tagged pointer into the area replaced by its label
 * (`code(label)` for the pointer), and a comment previewing the string
 * when the instruction refers to one - through its data operand or, for
 * "0B" store_inline, in its inline bytes */
static void WriteCode(Dis_t* d, uint32_t off, const AsmInsn_t* in)
{
	char text[0x400], lbl[32], mn[128];
	const char* comment = NULL;
	LinePrefix(d, off, in->len);
	if(in->targetKind)
	{ // the operand that is a target is printed as its label
		Label(d, in->target, lbl, sizeof lbl);
		Mnemonic(d, in, mn, sizeof mn);
		if(in->fam == 0 && (in->op == 0x15 || in->op == 0x3b) && in->count == 2)
			snprintf(text, sizeof text, "%s %d, %s", mn, (int)in->v[0], lbl); // jcc / jcmp: the condition first
		else
			snprintf(text, sizeof text, "%s %s", mn, lbl);
	}
	else if(in->fam == 0 && (in->op == 0x02 || in->op == 0x18) && CodePointer(d, (uint32_t)in->v[0]) >= 0 &&
		d->kind[CodePointer(d, (uint32_t)in->v[0])] != K_INSIDE)
	{ // (a tagged value pointing into the middle of an instruction is a constant, printed as it is)
		Label(d, (uint32_t)CodePointer(d, (uint32_t)in->v[0]), lbl, sizeof lbl);
		Mnemonic(d, in, mn, sizeof mn);
		snprintf(text, sizeof text, "%s code(%s)", mn, lbl);
	}
	else
		Asm_Format(in, d->gen, text, sizeof text);
	if(in->targetKind == 2)
	{
		uint32_t limit = in->target;
		while(limit < d->size && d->kind[limit] == K_DATA)
			limit++;
		if(StringLen(d, in->target, limit))
			comment = "string";
	}
	// store_inline whose bytes are exactly one NUL-terminated string
	if(in->fam == 0 && in->op == 0x0b && in->v[0] > 0 && StringLen(d, (uint32_t)(in->bytes - d->area), (uint32_t)(in->bytes - d->area) + (uint32_t)in->v[0]) == (uint32_t)in->v[0] - 1)
		comment = "inline";
	if(comment)
	{
		fprintf(d->out, "\t%-36s ; ", text);
		if(comment[0] == 's')
			Preview(d->out, d->area + in->target, StringLen(d, in->target, d->size));
		else
			Preview(d->out, in->bytes, (uint32_t)in->v[0] - 1);
		fputc('\n', d->out);
	}
	else
		fprintf(d->out, "\t%s\n", text);
}

/* write a run of data bytes [off, end) that has no label inside: a `.str`
 * line for each string (any string at a referenced offset, elsewhere one
 * of at least three characters), `.db` lines of up to 16 bytes for the
 * rest */
static void WriteData(Dis_t* d, uint32_t off, uint32_t end)
{
	while(off < end)
	{
		uint32_t n = StringLen(d, off, end);
		if(n && (d->label[off] & L_DATA || n >= 3))
		{
			LinePrefix(d, off, n + 1);
			fputs("\t.str ", d->out);
			Asm_WriteStringLiteral(d->out, d->area + off, n);
			fputc('\n', d->out);
			off += n + 1;
			continue;
		}
		{ // bytes up to the next string start or the end, 16 per line
			uint32_t stop = off + 1, i;
			while(stop < end && !(StringLen(d, stop, end) >= 3))
				stop++;
			while(off < stop)
			{
				uint32_t take = stop - off > 16 ? 16 : stop - off;
				LinePrefix(d, off, take);
				fputs("\t.db ", d->out);
				for(i = 0; i < take; i++)
					fprintf(d->out, "%s0x%02x", i ? ", " : "", d->area[off + i]);
				fputc('\n', d->out);
				off += take;
			}
		}
	}
}

/* Disassemble the program file into `out`: a heading, the `.engine` line,
 * then the area from start to end with labels, code and data.  Returns the
 * number of "; problem:" lines written, 0 for a clean listing.  A damaged
 * header is noted and the listing goes on with what the file holds. */
/* the analysis behind Dis_Program: the flows followed, the unreferenced
 * code found, every byte classified; the maps go to *out (Dis_FreeMap).
 * Problems are reported to `log` (NULL: not at all) and counted in
 * out->problems.  0 when the header is unusable (nothing is allocated). */
static int Analyze(Dis_t* d, const uint8_t* file, uint32_t size, EngineGen_t gen, FILE* log, uint32_t* codeOffsetOut)
{
	uint32_t codeOffset, codeSize, i;
	d->out = log;
	d->gen = gen;
	// "any" has no pointer layout of its own: the reference build's serves for code(label)
	d->profile = Engine_ProfileOfGen(gen == GEN_COUNT ? GEN_1_69_444 : gen);
	if(size < 16)
	{
		if(log)
			fprintf(log, "; problem: the file is shorter than its header\n");
		d->problems++;
		return 0;
	}
	// the header: the code offset (always 16) and the size of the area
	codeOffset = file[0] | (file[1] << 8) | (file[2] << 16) | ((uint32_t)file[3] << 24);
	codeSize = file[4] | (file[5] << 8) | (file[6] << 16) | ((uint32_t)file[7] << 24);
	if(codeOffset != 16 || codeOffset + codeSize > size)
	{
		if(log)
			fprintf(log, "; problem: header: code at 0x%x, size 0x%x in a file of 0x%x bytes\n", (unsigned)codeOffset,
				(unsigned)codeSize, (unsigned)size);
		d->problems++;
		if(codeOffset > size)
			return 0;
		if(codeOffset + codeSize > size)
			codeSize = size - codeOffset; // list what is there
	}
	if(codeOffset + codeSize < size && log)
	{
		fprintf(log, "; note: %u bytes follow the code area in the file and are not part of it\n",
			(unsigned)(size - codeOffset - codeSize));
	}
	*codeOffsetOut = codeOffset;
	d->area = file + codeOffset;
	d->size = codeSize;
	d->kind = (uint8_t*)calloc(codeSize + 1, 1); // one entry more than the area: a branch may target its end
	d->label = (uint8_t*)calloc(codeSize + 1, 1);
	// the flows: from the entry, then from every target found on the way
	Push(d, 0);
	while(d->workCount)
		Follow(d, d->work[--d->workCount]);
	FindUnreferencedCode(d);
	for(i = 0; i < d->size; i++)
		if(d->kind[i] == K_UNKNOWN)
			d->kind[i] = K_DATA; // whatever no flow reached
	free(d->work);
	d->work = NULL;
	return 1;
}

int Dis_Analyze(const uint8_t* file, uint32_t size, EngineGen_t gen, FILE* log, DisMap_t* out)
{
	Dis_t d;
	uint32_t codeOffset = 16;
	memset(&d, 0, sizeof d);
	memset(out, 0, sizeof *out);
	if(!Analyze(&d, file, size, gen, log, &codeOffset))
	{
		out->problems = d.problems;
		return 0;
	}
	out->area = d.area;
	out->size = d.size;
	out->kind = d.kind;
	out->label = d.label;
	out->problems = d.problems;
	return 1;
}

void Dis_FreeMap(DisMap_t* m)
{
	free(m->kind);
	free(m->label);
	memset(m, 0, sizeof *m);
}

// a Dis_t over an analysed map, for the writers
static void FromMap(Dis_t* d, const DisMap_t* m, EngineGen_t gen, const DisOptions_t* opt, FILE* out)
{
	memset(d, 0, sizeof *d);
	d->area = m->area;
	d->size = m->size;
	d->kind = m->kind;
	d->label = m->label;
	d->gen = gen;
	d->profile = Engine_ProfileOfGen(gen == GEN_COUNT ? GEN_1_69_444 : gen);
	d->opt = opt;
	d->out = out;
}

void Dis_WriteRange(const DisMap_t* m, uint32_t start, uint32_t end, EngineGen_t gen, const DisOptions_t* opt, FILE* out)
{
	Dis_t d;
	uint32_t off = start;
	char lbl[32];
	FromMap(&d, m, gen, opt, out);
	if(end > d.size)
		end = d.size;
	while(off < end)
	{
		if(d.label[off])
		{
			Label(&d, off, lbl, sizeof lbl);
			fprintf(out, "%s:%s\n", lbl, (d.label[off] & L_DEAD) ? " ; unreferenced" : "");
		}
		if(d.kind[off] == K_CODE)
		{
			AsmInsn_t in;
			Asm_Decode(d.area, d.size, off, d.gen, &in);
			WriteCode(&d, off, &in);
			off += in.len;
		}
		else
		{ // data up to the next label or code
			uint32_t stop = off + 1;
			while(stop < end && d.kind[stop] == K_DATA && !d.label[stop])
				stop++;
			WriteData(&d, off, stop);
			off = stop;
		}
	}
}

void Dis_LabelName(const DisMap_t* m, uint32_t off, char* buf, size_t n)
{
	Dis_t d;
	FromMap(&d, m, GEN_COUNT, NULL, NULL);
	Label(&d, off, buf, n);
}

uint32_t Dis_StringLength(const DisMap_t* m, uint32_t off)
{
	Dis_t d;
	uint32_t limit = off;
	FromMap(&d, m, GEN_COUNT, NULL, NULL);
	while(limit < d.size && d.kind[limit] == K_DATA)
		limit++;
	return StringLen(&d, off, limit);
}

int Dis_Program(const uint8_t* file, uint32_t size, const DisOptions_t* opt, FILE* out)
{
	DisMap_t m;
	fprintf(out, "; openbgi disassembly%s%s\n", opt->name ? " of " : "", opt->name ? opt->name : "");
	if(!Dis_Analyze(file, size, opt->gen, out, &m))
		return m.problems;
	fprintf(out, ".engine %s\n", opt->gen == GEN_COUNT ? "any" : kGenNames[opt->gen]);
	fprintf(out, "; %u bytes of code area; the entry is offset 0\n\n", (unsigned)m.size);
	Dis_WriteRange(&m, 0, m.size, opt->gen, opt, out);
	Dis_FreeMap(&m);
	return m.problems;
}
