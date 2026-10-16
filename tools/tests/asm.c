/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/asm.c - unit test of the assembler and disassembler (src/asm/,
 *               declared in inc/bgi/asm.h); built as bin/test_asm by
 *               `make test`
 *
 * The groups of tests, in the order they run:
 *
 *   encodings  - snippets are assembled and their bytes compared with the
 *                encodings the VM's ops_base.c decodes: operand sizes,
 *                family prefixes, relative targets, code() pointers, the
 *                data directives, Shift-JIS conversion and the 1.667 forms
 *   names      - mnemonic lookup in both directions, including opcodes
 *                that were renumbered between generations
 *   round trip - a program with every operand kind, forward and backward
 *                labels, strings and data is disassembled and assembled
 *                again; the bytes have to be identical, with and without
 *                the offset and byte columns in the listing
 *   run        - an assembled program is executed by the VM's base
 *                instruction table and has to leave the expected result
 *   errors     - texts that must be rejected, and a warning that must not
 *                be an error
 *
 * Every check prints its file and line on failure; main exits with 1 when
 * any check failed.
 */
#include "bgi/asm.h"
#include "bgi/vm.h"
#include "bgi/sys.h"
#include "bgi/os.h"

#include <stdio.h>

static int gFails; // the number of failed checks so far
// evaluate a condition; print it with its location and count a failure when it is false
#define CHECK(c)                                                  \
	do                                                            \
	{                                                             \
		if(!(c))                                                  \
		{                                                         \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			gFails++;                                             \
		}                                                         \
	} while(0)

/* Assemble `text` for generation `gen` and copy the program area (the
 * file without its 16-byte header) into `out`, at most `cap` bytes.
 * Returns the full size of the area, or 0 when the assembler reported an
 * error.  With `quiet` the assembler's messages are discarded, for the
 * tests that expect an error. */
static uint32_t Assemble(const char* text, EngineGen_t gen, uint8_t* out, uint32_t cap, int quiet)
{
	AsmResult_t r;
	uint32_t n;
	FILE* log = quiet ? fopen("/dev/null", "w") : stderr;
	Asm_Assemble(text, strlen(text), gen, log, &r);
	if(quiet)
		fclose(log);
	if(!r.file)
		return 0;
	n = r.size - 16;
	if(n > cap)
		n = cap;
	memcpy(out, r.file + 16, n);
	BGI_Free(r.file);
	return r.size - 16;
}

/* Compare `n` bytes with a string of space-separated two-digit hex
 * values ("00 ff 01").  Returns 1 only when every byte matches and the
 * string lists exactly `n` bytes, so a wrong length fails too. */
static int Bytes(const uint8_t* got, uint32_t n, const char* hex)
{
	uint32_t i;
	for(i = 0; i < n; i++)
	{
		unsigned v;
		if(sscanf(hex + i * 3, "%2x", &v) != 1 || v != got[i])
			return 0;
	}
	return strlen(hex) == n * 3 - 1;
}

/* The instruction encodings: each snippet covers one group of operand
 * kinds and the expected bytes are the ones the VM decodes.  A failure
 * means the assembler emits an instruction the VM would misread (or that
 * a mnemonic maps to the wrong opcode). */
static void TestEncodings(void)
{
	uint8_t b[0x100];
	uint32_t n;
	printf("encodings\n");
	// the immediate operands: i8, i16, i32 and a local address
	n = Assemble("push_i8 -1\npush_i16 300\npush_i32 0x12345678\npush_local_addr 0x250\n", GEN_1_69_444, b, sizeof b, 0);
	CHECK(n == 13 && Bytes(b, n, "00 ff 01 2c 01 02 78 56 34 12 04 50 02"));
	// the memory and control instructions with their size / count bytes
	n = Assemble("load 2\nstore_keep 1\nstore 0\nstore_multi 2, 3\njcc 5\njmp\ncall\nret\n", GEN_1_69_444, b, sizeof b, 0);
	CHECK(n == 14 && Bytes(b, n, "08 02 09 01 0a 00 0c 02 03 15 05 14 16 17"));
	// store_inline: the byte count precedes the inline bytes; without operands it is a
	// count of 0
	n = Assemble("store_inline 1, 2, \"ab\"\nstore_inline\n", GEN_1_69_444, b, sizeof b, 0);
	CHECK(n == 8 && Bytes(b, n, "0b 04 01 02 61 62 0b 00"));
	// the family prefixes, by name and by number
	n = Assemble("sys.yield\nsys.0x5e\ngfx0.present\nuser.0x12\nsnd.0x00\n", GEN_1_69_444, b, sizeof b, 0);
	CHECK(n == 10 && Bytes(b, n, "80 5f 80 5e 90 00 ff 12 a0 00"));
	// the arithmetic, comparison and logic instructions have no operands
	n = Assemble("add\nsub\nmul\ndiv\nmod\nand\nor\nxor\nnot\nshl\nshr\nsar\neq\nne\nle\nge\nlt\ngt\nland\nlor\nlnot\nselect\nmuldiv\nsin\ncos\n", GEN_1_69_444, b, sizeof b, 0);
	CHECK(n == 25 && Bytes(b, n, "20 21 22 23 24 25 26 27 28 29 2a 2b 30 31 32 33 34 35 38 39 3a 40 42 48 49"));
	// relative operands count from the opcode's own offset
	n = Assemble("start:\npush_code_off start\npush_code_addr s\npush_code_off end\ns:\n.str \"x\"\nend:\n", GEN_1_69_444, b, sizeof b, 0);
	CHECK(n == 11 && Bytes(b, n, "06 00 00 05 06 00 06 05 00 78 00"));
	// the convenience forms: `push` picks the smallest immediate, `jmp`/`call`/`jcc` with
	// a label push the target first
	n = Assemble("push 5\npush 300\npush 70000\npush -129\njmp l\ncall l\njcc 1, l\nl:\n", GEN_1_69_444, b, sizeof b, 0);
	CHECK(n == 26 && Bytes(b, n, "00 05 01 2c 01 02 70 11 01 00 01 7f ff 06 0d 00 14 06 09 00 16 06 05 00 15 01"));
	// code(): a tagged pointer in the generation's layout (1.69/444: tag 8 << 25 =
	// 0x10000000)
	n = Assemble("push_i32 code(d)\nd:\n.db 1\n", GEN_1_69_444, b, sizeof b, 0);
	CHECK(n == 6 && Bytes(b, n, "02 05 00 00 10 01"));
	// a generation with another tag layout: the tag byte follows its engine profile
	n = Assemble("push_i32 code(d)\nd:\n.db 1\n", GEN_1_669, b, sizeof b, 0);
	CHECK(n == 6 && b[4] == (uint8_t)(Engine_ProfileOfGen(GEN_1_669)->tagCode << (Engine_ProfileOfGen(GEN_1_669)->tagShift - 24)));
	// the data directives: .db takes numbers, characters and strings; .align pads with 0
	// or the given byte; .str is NUL-terminated
	n = Assemble(".db 1, 2, 'A', \"hi\"\n.dw 0x1234, -1\n.dd 0x80000001\n.align 4\n.align 4, 0xff\n.str \"a\", \"b\"\n", GEN_1_69_444, b, sizeof b, 0);
	CHECK(n == 20 && Bytes(b, n, "01 02 41 68 69 34 12 ff ff 01 00 00 80 00 00 00 61 00 62 00"));
	// Shift-JIS from UTF-8, and escapes
	n = Assemble(".str \"あ\\x81\\x40\\n\\\"\\\\\"\n", GEN_1_69_444, b, sizeof b, 0);
	CHECK(n == 8 && Bytes(b, n, "82 a0 81 40 0a 22 5c 00"));
	// the 1.667 forms: packed local references, varint immediates and rel16 targets
	n = Assemble("store_local 0x10, 2\njmp_rel t\npush_local 4, 0\npush_local_add 8, 0, -3\npush_local_shl 4, 1, 1\nload_rel 300\nadd_imm -64\njcmp 3, t\njcc 9, t\nshift_imm -2\nt:\n", GEN_1_667, b, sizeof b, 0);
	CHECK(n == 31);
	CHECK(b[0] == 0x0f && b[1] == 0x10 && b[2] == 0x80);                     // local16: offset 0x10, size 2 in bits 14..15
	CHECK(b[3] == 0x12);                                                     // jmp_rel
	CHECK(b[5] == 0x19 && b[6] == 0x04 && b[7] == 0x00);                     // push_local 4, 0
	CHECK(b[8] == 0x1a && b[11] == 0x7d);                                    // -3 as a varint
	CHECK(b[12] == 0x1c && b[13] == 0x04 && b[14] == 0x40 && b[15] == 0x01); // push_local_shl 4, 1, 1
	CHECK(b[16] == 0x1f && b[17] == 0xac && b[18] == 0x02);                  // 300 as a varint
	CHECK(b[19] == 0x2c && b[20] == 0x40);                                   // -64 as a varint (one byte, bit 6 set)
	CHECK(b[21] == 0x3b && b[22] == 0x03);                                   // jcmp 3, rel16 from its own offset
	CHECK(b[25] == 0x15 && b[26] == 0x09);                                   // jcc 9, rel16
	CHECK(b[29] == 0x3c && b[30] == 0xfe);
	{ // the relative targets of the 1.667 forms decode back to the label's offset (31, the end)
		AsmInsn_t in;
		CHECK(Asm_Decode(b, n, 3, GEN_1_667, &in) && in.op == 0x12 && in.targetKind == 1 && in.target == 31);
		CHECK(Asm_Decode(b, n, 21, GEN_1_667, &in) && in.op == 0x3b && in.count == 2 && in.target == 31);
		CHECK(Asm_Decode(b, n, 25, GEN_1_667, &in) && in.op == 0x15 && in.count == 2 && in.target == 31);
		CHECK(Asm_Decode(b, n, 25, GEN_1_69_444, &in) && in.count == 1 && in.len == 2); // not in an older build: "15" is a plain jcc there
	}
}

/* Mnemonic lookup (Asm_LookupName, Asm_OpName, Asm_FamilyName): names and
 * numeric forms, unknown names and families, and an opcode whose number
 * differs between generations.  A failure means a listing would assemble
 * to another instruction than it names, or disassemble under the wrong
 * name. */
static void TestNames(void)
{
	int fam, op;
	printf("names\n");
	CHECK(Asm_LookupName("push_i8", 7, GEN_1_69_444, &fam, &op) && fam == 0 && op == 0x00);
	CHECK(Asm_LookupName("sys.yield", 9, GEN_1_69_444, &fam, &op) && fam == 0x80 && op == 0x5f);
	CHECK(Asm_LookupName("sys.0x5F", 8, GEN_1_69_444, &fam, &op) && fam == 0x80 && op == 0x5f);
	CHECK(!Asm_LookupName("sys.nothing_here", 16, GEN_1_69_444, &fam, &op));
	CHECK(!Asm_LookupName("nofam.yield", 11, GEN_1_69_444, &fam, &op));
	// a renumbered instruction: its name follows the generation
	CHECK(Asm_LookupName("gfx0.window_set_punch", 21, GEN_1_64, &fam, &op) && op == 0x8b);
	CHECK(Asm_LookupName("gfx0.window_set_punch", 21, GEN_1_66, &fam, &op) && op == 0x87);
	CHECK(strcmp(Asm_OpName(0x90, 0x8b, GEN_1_64), "window_set_punch") == 0);
	CHECK(strcmp(Asm_OpName(0x80, 0x63, GEN_1_69_444), "set_window_pos_mode") == 0);
	CHECK(strcmp(Asm_FamilyName(0x91), "gfx1") == 0 && Asm_FamilyName(0x82) == NULL);
}

/* The round trip: a 1.553 program using every operand kind (frame set-up,
 * a string reference, a tagged code() pointer, forward and backward
 * labels, inline bytes, a string and a data table) is assembled, wrapped
 * in a program header, disassembled with Dis_Program and assembled again.
 * The listing has to name what it found (sub_, loc_, str_, dat_ labels,
 * the string, the code() form, the system mnemonics) and the second
 * assembly has to reproduce the bytes exactly, both from a bare listing
 * and from one with the offset and byte columns.  A failure means the
 * disassembler writes something the assembler reads differently. */
static void TestRoundTrip(void)
{
	static const char* src =
		".engine 1.553\n"
		"entry:\n"
		"\tpush_fp\n"
		"\tpush_i16 600\n"
		"\tadd\n"
		"\tset_fp\n"
		"\tpush_code_addr str_hello\n"
		"\tsys.set_title\n"
		"\tpush_i8 0\n"
		"\tpush_code_off loop\n"
		"\tjcc 1\n"
		"loop:\n"
		"\tpush_local_addr 0x10\n"
		"\tpush_i32 code(table)\n"
		"\tstore 2\n"
		"\tpush_code_off sub\n"
		"\tcall\n"
		"\tpush_code_off loop\n"
		"\tjmp\n"
		"sub:\n"
		"\tstore_inline 0x01, 0x02\n"
		"\tstore_multi 2, 2\n"
		"\tsys.exit_thread\n"
		"str_hello:\n"
		"\t.str \"こんにちは\"\n" // "hello"
		"table:\n"
		"\t.db 0x01, 0x02, 0x03, 0x04\n";
	uint8_t a[0x200], b[0x200];
	uint32_t na, nb;
	FILE* tmp = tmpfile();
	char text[0x1000];
	long n;
	DisOptions_t opt;
	uint8_t* file;
	printf("round trip\n");
	na = Assemble(src, GEN_1_553, a, sizeof a, 0);
	CHECK(na > 0);
	// a minimal 16-byte program header: the code offset (16) and the little-endian size
	// of the area
	file = (uint8_t*)BGI_Alloc(16 + na);
	memset(file, 0, 16);
	file[0] = 0x10;
	file[4] = (uint8_t)na;
	file[5] = (uint8_t)(na >> 8);
	memcpy(file + 16, a, na);
	memset(&opt, 0, sizeof opt);
	opt.gen = GEN_1_553;
	CHECK(Dis_Program(file, 16 + na, &opt, tmp) == 0);
	n = ftell(tmp);
	fseek(tmp, 0, SEEK_SET);
	CHECK(n > 0 && n < (long)sizeof text);
	text[fread(text, 1, sizeof text - 1, tmp)] = 0;
	fclose(tmp);
	// the listing names what it found
	CHECK(strstr(text, "sub_") != NULL && strstr(text, "loc_") != NULL && strstr(text, "str_") != NULL &&
		strstr(text, "dat_") != NULL);
	CHECK(strstr(text, ".str \"こんにちは\"") != NULL);
	CHECK(strstr(text, "code(dat_") != NULL);
	CHECK(strstr(text, "sys.set_title") != NULL && strstr(text, "sys.exit_thread") != NULL);
	nb = Assemble(text, GEN_1_553, b, sizeof b, 0);
	CHECK(nb == na && memcmp(a, b, na) == 0);
	BGI_Free(file);
	// a listing with the offset and byte columns assembles too (the assembler skips
	// everything up to the '|')
	tmp = tmpfile();
	opt.showBytes = opt.showOffsets = 1;
	file = (uint8_t*)BGI_Alloc(16 + na);
	memset(file, 0, 16);
	file[0] = 0x10;
	file[4] = (uint8_t)na;
	file[5] = (uint8_t)(na >> 8);
	memcpy(file + 16, a, na);
	CHECK(Dis_Program(file, 16 + na, &opt, tmp) == 0);
	n = ftell(tmp);
	fseek(tmp, 0, SEEK_SET);
	text[fread(text, 1, sizeof text - 1, tmp)] = 0;
	fclose(tmp);
	CHECK(strstr(text, "|\tpush_fp") != NULL);
	nb = Assemble(text, GEN_1_553, b, sizeof b, 0);
	CHECK(nb == na && memcmp(a, b, na) == 0);
	BGI_Free(file);
}

/* Execution: an assembled program is loaded into a fresh thread and
 * stepped through the base instruction table until a handler returns a
 * non-zero scheduler code.  The program reserves a frame, computes 6 * 7
 * into a local, loads it back and returns, so the thread has to end with
 * the single value 42 on its stack.  A failure means the assembler's
 * output and the VM's decoder disagree on a real program. */
static void TestRun(void)
{
	// a program the VM runs: 6 * 7 into a local, then onto the stack
	static const char* src =
		"\tpush_fp\n"
		"\tpush_i8 8\n"
		"\tadd\n"
		"\tset_fp\n"
		"\tpush 6\n"
		"\tpush 7\n"
		"\tmul\n"
		"\tpush_local_addr 4\n"
		"\tstore 2\n"
		"\tpush_local_addr 4\n"
		"\tload 2\n"
		"\tpush_fp\n"
		"\tpush_i8 8\n"
		"\tsub\n"
		"\tset_fp\n"
		"\tret\n";
	uint8_t code[0x100];
	uint32_t n = Assemble(src, GEN_1_69_444, code, sizeof code, 0);
	Thread_t* t;
	int rc = 0, steps = 0;
	printf("run\n");
	CHECK(n > 0);
	Vm_TablesInit();
	t = Thread_New(0x100, 0x1000, 0x1000);
	memcpy(t->code, code, n);
	t->codeUsed = n;
	Thread_SetIP(t, 0);
	Thread_SetFP(t, 0);
	t->sp = 0;
	// step the base table by hand; the step limit guards against a runaway program
	while(rc == 0 && steps++ < 1000)
	{
		uint8_t op = Thread_FetchOp(t);
		rc = vm_optable_main[op] ? vm_optable_main[op](t) : -1;
	}
	CHECK(rc == 4); // the outermost return ends the thread
	CHECK(t->sp == 1 && t->stack[0] == 42);
	Thread_Delete(t);
}

/* The error paths: each text has to be rejected (Assemble returns 0): an
 * undefined label, an immediate out of range, an unknown mnemonic, a
 * duplicate label, a directive with the wrong operand kind, an operand on
 * an instruction that takes none, an unknown .engine version and a
 * relative target beyond the reach of a 16-bit offset.  An opcode that
 * does not exist in the chosen generation is only a warning and still
 * assembles.  A failure means bad input is accepted silently, or that the
 * warning became an error. */
static void TestErrors(void)
{
	uint8_t b[0x100];
	printf("errors\n");
	CHECK(Assemble("push_code_off nowhere\n", GEN_1_69_444, b, sizeof b, 1) == 0);
	CHECK(Assemble("push_i8 300\n", GEN_1_69_444, b, sizeof b, 1) == 0);
	CHECK(Assemble("bogus_mnemonic\n", GEN_1_69_444, b, sizeof b, 1) == 0);
	CHECK(Assemble("l:\nl:\n", GEN_1_69_444, b, sizeof b, 1) == 0);
	CHECK(Assemble(".str 5\n", GEN_1_69_444, b, sizeof b, 1) == 0);
	CHECK(Assemble("sys.yield 1\n", GEN_1_69_444, b, sizeof b, 1) == 0);
	CHECK(Assemble(".engine 9.99\n", GEN_1_69_444, b, sizeof b, 1) == 0);
	// a far target: 9000 five-byte instructions put the label beyond a 16-bit relative
	// offset
	{
		char* big = (char*)malloc(0x20000);
		size_t o = (size_t)sprintf(big, "push_code_off far\n");
		int i;
		for(i = 0; i < 9000; i++)
			o += (size_t)sprintf(big + o, "push_i32 0\n");
		sprintf(big + o, "far:\n");
		CHECK(Assemble(big, GEN_1_69_444, b, sizeof b, 1) == 0);
		free(big);
	}
	// an opcode outside the generation's set assembles with a warning (atan2 did not
	// exist in 1.58)
	{
		AsmResult_t r;
		FILE* log = fopen("/dev/null", "w");
		Asm_Assemble("atan2\n", 5, GEN_1_58, log, &r);
		fclose(log);
		CHECK(r.file != NULL && r.warnings == 1 && r.errors == 0);
		BGI_Free(r.file);
	}
}

// run every group; exit status 1 when any check failed
int main(void)
{
	OS_Init();
	TestEncodings();
	TestNames();
	TestRoundTrip();
	TestRun();
	TestErrors();
	OS_Shutdown();
	if(gFails)
	{
		printf("%d failure(s)\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
