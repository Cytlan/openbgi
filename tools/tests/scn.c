/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/scn.c - unit test of the scenario decompiler and compiler
 *               (src/scn/, declared in inc/bgi/scn.h); built as
 *               bin/test_scn by `make test`
 *
 * The groups of tests, in the order they run:
 *
 *   container  - a file image is built by hand, loaded, and written back
 *                identically; the header padding rule; malformed files
 *   compile    - sources of both styles compile to the tokens the
 *                original compilers write: line markers, the argument
 *                orders, the frame of a function, the label dispatcher
 *   round trip - the compiled files are decompiled and compiled again
 *                with the same bytes; the decompiled source is checked for
 *                the constructs it should contain (if / else / while /
 *                switch, msg, jump, labels)
 *   raw        - the headerless variant of the 1.66 / 1.69 games: the
 *                layout, the calls by address, the byte parameters, the
 *                #file and #string directives, the function list
 *   16-bit     - a scene of the 1.64 games: the opcodes and operands it
 *                compiles to, the text table, the labels; the round trip;
 *                malformed files and sources
 *   errors     - texts the compiler must reject
 *
 * The game commands used are given by a small table built in the test
 * (the real one comes from a game's sysprg.arc, Scn_ScanCommands).
 */
#include "bgi/scn.h"
#include "bgi/os.h"

#include <stdio.h>

static int gFails; // the number of failed checks so far
#define CHECK(c)                                                  \
	do                                                            \
	{                                                             \
		if(!(c))                                                  \
		{                                                         \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			gFails++;                                             \
		}                                                         \
	} while(0)

// a command table with the handful of commands the tests use
static ScnCmd_t gCmdList[] = {
	{0x0e0, "scrctrl", "set_lwork", 2, 0, 0, ""},
	{0x0e1, "scrctrl", "lwork", 1, 1, 0, ""},
	{0x0e6, "scrctrl", "set_gflag", 2, 0, 0, ""},
	{0x0e7, "scrctrl", "gflag", 1, 1, 0, ""},
	{0x0e8, "scrctrl", "lstr", 1, 1, 0, ""},
	{0x0f0, "scrctrl", "call_scenario", 1, 0, 0, ""},
	{0x0f3, "scrctrl", "jump_scenario", 1, 0, 0, ""},
	{0x0f4, "scrctrl", "quit", 0, 0, 0, ""},
	{0x0f6, "scrctrl", "set_label", 1, 0, 0, ""},
	{0x0f7, "scrctrl", "test_label", 1, 1, 0, ""},
	{0x0f9, "scrctrl", "msgbox", 1, 0, 0, ""},
	{0x118, "scrsys", "set_scene_name", 1, 0, 0, ""},
	{0x128, "scrsys", "sys_128", 0, 1, 0, ""},
	{0x140, "scrmsg", "msg", 5, 0, 0, ""},
	{0x1a9, "scrsnd", "voice", 1, 0, 0, ""},
};
static ScnCmdTable_t gCmds = {gCmdList, (int)(sizeof gCmdList / sizeof gCmdList[0]), 0, 0};

static uint32_t Rd32(const uint8_t* p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

// compile `text` and hand back the file image (BGI_Free it); NULL on errors (reported when `quiet` is 0)
static uint8_t* Compile(const char* text, uint32_t* size, int quiet)
{
	ScnCompileResult_t r;
	uint8_t* out;
	FILE* log = quiet ? fopen("/dev/null", "w") : stderr;
	Scn_Compile(text, strlen(text), "test", &gCmds, NULL, log, &r);
	if(quiet)
		fclose(log);
	if(r.errors)
		return NULL;
	out = Scn_Save(&r.file, size);
	Scn_Free(&r.file);
	return out;
}

// decompile an image into a malloc'ed string (NULL on a load error)
static char* Decompile(const uint8_t* data, uint32_t size, int* fallbacks)
{
	ScnFile_t f;
	ScnDecOptions_t opt;
	char err[128];
	FILE* mem = tmpfile();
	long n;
	char* text;
	if(!Scn_Load(data, size, &f, err, sizeof err))
	{
		fclose(mem);
		return NULL;
	}
	memset(&opt, 0, sizeof opt);
	opt.cmds = &gCmds;
	opt.name = "test";
	*fallbacks = Scn_Decompile(&f, &opt, mem);
	Scn_Free(&f);
	n = ftell(mem);
	fseek(mem, 0, SEEK_SET);
	text = (char*)malloc((size_t)n + 1);
	if(fread(text, 1, (size_t)n, mem) != (size_t)n)
		n = 0;
	text[n] = 0;
	fclose(mem);
	return text;
}

// the token at dword index i of an image
static uint32_t Tok(const uint8_t* img, uint32_t i)
{
	uint32_t base = 0x1c + Rd32(img + 0x1c);
	return Rd32(img + base + 4 * i);
}

// the dword index of the first token `tok` (operands skipped) from dword `i` within the first `limit` dwords, or `limit`
static uint32_t FindTok(const uint8_t* img, uint32_t tok, uint32_t i, uint32_t limit)
{
	while(i < limit && Tok(img, i) != tok)
		i += 1 + (uint32_t)Scn_OperandCount(Tok(img, i));
	return i < limit ? i : limit;
}

static void TestContainer(void)
{
	// a file with one import, one export and two tokens (push 5; ret), one string
	static const uint8_t img[] = {'B', 'u', 'r', 'i', 'k', 'o', 'C', 'o', 'm', 'p', 'i', 'l', 'e', 'd', 'S', 'c', 'r', 'i', 'p', 't',
		'V', 'e', 'r', '1', '.', '0', '0', 0,
		0x24, 0, 0, 0,                                 // header size: the rest is 0x24 bytes (to offset 0x40)
		1, 0, 0, 0, 'f', 'w', 0,                       // one import "fw"
		1, 0, 0, 0, 'm', 'a', 'i', 'n', 0, 0, 0, 0, 0, // one export "main" at 0
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,            // padding to 0x40
		3, 0, 0, 0, 12, 0, 0, 0,                       // str 12 (the string table starts at byte 12 of the stream)
		0x1b, 0, 0, 0,                                 // ret
		'h', 'i', 0};
	ScnFile_t f;
	char err[128];
	uint8_t* back;
	uint32_t size;
	CHECK(sizeof img == 0x40 + 12 + 3);
	CHECK(Scn_Load(img, sizeof img, &f, err, sizeof err));
	CHECK(f.importCount == 1 && strcmp(f.imports[0], "fw") == 0);
	CHECK(f.exportCount == 1 && strcmp(f.exports[0].name, "main") == 0 && f.exports[0].off == 0);
	CHECK(f.codeLen == 3 && f.code[0] == SCN_STR && f.code[1] == 12 && f.code[2] == SCN_RET);
	CHECK(f.stringsLen == 3 && strcmp(Scn_String(&f, 12), "hi") == 0);
	CHECK(Scn_String(&f, 11) == NULL && Scn_String(&f, 15) == NULL);
	back = Scn_Save(&f, &size);
	CHECK(size == sizeof img && memcmp(back, img, size) == 0);
	BGI_Free(back);
	Scn_Free(&f);
	// the padding rule: a header that ends on a multiple of 16 still gets 16 bytes
	{
		ScnFile_t g;
		memset(&g, 0, sizeof g);
		strcpy(g.imports[0], "abcdefghijk"); // 0x24 + 12 + 4 = 0x34 -> 12 bytes of padding
		g.importCount = 1;
		back = Scn_Save(&g, &size);
		CHECK(Rd32(back + 0x1c) == 0x40 - 0x1c && size == 0x40);
		BGI_Free(back);
		strcpy(g.imports[0], "abcdefghijklmno"); // 0x24 + 16 + 4 = 0x38 -> 8
		back = Scn_Save(&g, &size);
		CHECK(size == 0x40);
		BGI_Free(back);
		strcpy(g.imports[0], "abcdefghijklmnopqrstuvw"); // 0x24 + 24 + 4 = 0x40 -> 16
		back = Scn_Save(&g, &size);
		CHECK(size == 0x50);
		BGI_Free(back);
	}
	// malformed: a bad magic, a token running past the end
	{
		uint8_t bad[sizeof img];
		memcpy(bad, img, sizeof img);
		bad[0] = 'X';
		CHECK(!Scn_Load(bad, sizeof img, &f, err, sizeof err));
		memcpy(bad, img, sizeof img);
		bad[0x40] = 0; // push with its operand being the ret: the stream then runs into the strings
		CHECK(!Scn_Load(bad, sizeof img, &f, err, sizeof err));
	}
	CHECK(Scn_OperandCount(SCN_SRCINFO) == 3 && Scn_OperandCount(SCN_LINE) == 2 && Scn_OperandCount(0x140) == 0);
}

static const char* kScenario = "#style scenario\n"
							   "#import \"framework\"\n"
							   "#source \"t.txt\"\n"
							   "#line 1\n"
							   "set_scene_name(\"intro\");\n"
							   "_Wait(2000);\n"
							   "if (gflag(240)) {\n"
							   "    set_gflag(240, 0);\n"
							   "} else {\n"
							   "    set_lwork(1, 5 + -3);\n"
							   "}\n"
							   "voice(\"a_01\"); msg(\"Hello\", \"Name\", 0, 1, 1);\n"
							   "start:\n"
							   "if (lwork(1) == 5) {\n"
							   "    goto start;\n"
							   "}\n"
							   "call sub;\n"
							   "lstr(0) = \"bg01\";\n"
							   "lstr(1) = lstr(0) + \"_n\" + 2;\n"
							   ";\n"
							   "set_lwork!(2, lwork!(1));\n"
							   "_Draw(lstr(0), 1);\n"
							   "jump \"_02\", \"top\";\n"
							   "sub:\n"
							   "end;\n";

// the older scenario variant: `line` markers (also before an if), goto through the dispatcher, msg in the args form
static const char* kScenarioLine = "#style scenario-line\n"
								   "#import \"function\"\n"
								   "#source \"D:\\\\Script\\\\01.bsb\"\n"
								   "#line 1\n"
								   "set_scene_name(\"intro\");\n"
								   "fnDraw(1, 2);\n"
								   "msg(\"Hello\", \"Name\", 0, 1, 1);\n"
								   "if (lwork(0) == 1) {\n"
								   "    goto next;\n"
								   "} else if (lwork(0) == 2) {\n"
								   "    goto next;\n"
								   "}\n"
								   "next:\n"
								   "jump \"02\";\n";

static const char* kLibrary = "#style library\n"
							  "#import \"function\"\n"
							  "#source \"Lib.bsc\"\n"
							  "#include \"bss.h\"\n"
							  "#line 1\n"
							  "\n"
							  "_Second(p1, char* p2) { int a; char buf[256]; int arr[4];\n"
							  "    a = 0;\n"
							  "    while (a < 4) {\n"
							  "        arr[a] = p1 * a;\n"
							  "        a = a + 1;\n"
							  "    }\n"
							  "    if (p2[0] == 95) {\n"
							  "        return arr[2];\n"
							  "    }\n"
							  "    switch (p1) {\n"
							  "        case 0:\n"
							  "            set_lwork(0, 1);\n"
							  "            break;\n"
							  "        case 1:\n"
							  "        default:\n"
							  "            _Second(p1 - 1, p2);\n"
							  "            break;\n"
							  "    }\n"
							  "    return -a;\n"
							  "}\n"
							  "main() {\n"
							  "    if (sys_128()) {\n"
							  "        _Second(1, \"x\");\n"
							  "    } else {\n"
							  "        if (lwork(2)) {\n"
							  "            _Second(2, \"y\");\n"
							  "        } else {\n"
							  "            set_lwork(2, 1);\n"
							  "        }\n"
							  "    }\n"
							  "}\n";

static void TestCompile(void)
{
	uint32_t size, base, i, n;
	uint8_t* img = Compile(kScenario, &size, 0);
	CHECK(img != NULL);
	if(!img)
		return;
	base = 0x1c + Rd32(img + 0x1c);
	CHECK(base == 0x40); // 0x24 + "framework\0" (10) + 4 = 0x32 -> 14 bytes of padding
	// the entry jump, then the first statement's marker: str "t.txt"; push 1; args 2; lineinfo
	CHECK(Tok(img, 0) == SCN_ADDR && Tok(img, 2) == SCN_JMP);
	CHECK(Tok(img, 3) == SCN_STR && Tok(img, 5) == SCN_PUSH && Tok(img, 6) == 1 && Tok(img, 7) == SCN_ARGS && Tok(img, 8) == 2 && Tok(img, 9) == SCN_LINEINFO);
	// set_scene_name("intro"): str; args 1; 0x118
	CHECK(Tok(img, 10) == SCN_STR && Tok(img, 12) == SCN_ARGS && Tok(img, 13) == 1 && Tok(img, 14) == 0x118);
	// the string table starts with the source name, then the strings in order of use
	{
		const char* s = (const char*)img + base;
		uint32_t strStart = Tok(img, 4); // the first str operand: the source name begins the table
		CHECK(strcmp(s + strStart, "t.txt") == 0);
		CHECK(strcmp(s + strStart + 6, "intro") == 0);
	}
	// the user function call: args left to right, the name, callfn
	for(i = 0, n = 0; i < 400 && base + 4 * i + 4 <= size; i++)
		if(Tok(img, i) == SCN_CALLFN)
			n++;
	CHECK(n == 2);
	// msg: the direct form, the arguments right to left so that the text ends on top, no args token
	i = FindTok(img, 0x140, 0, 400);
	CHECK(i < 400 && Tok(img, i - 2) == SCN_STR && Tok(img, i - 4) == SCN_STR && Tok(img, i - 6) == SCN_PUSH && Tok(img, i - 5) == 0 && Tok(img, i - 10) == SCN_PUSH && Tok(img, i - 9) == 1);
	{
		const char* s = (const char*)img + base;
		CHECK(strcmp(s + Tok(img, i - 1), "Hello") == 0 && strcmp(s + Tok(img, i - 3), "Name") == 0);
	}
	// set_lwork!(2, lwork!(1)): push 1; lwork; push 2; set_lwork - nothing reversed, no args tokens
	i = FindTok(img, 0xe0, FindTok(img, 0x94, 0, 400), 400); // the second set_lwork, after the string assignment
	CHECK(i < 400 && Tok(img, i - 2) == SCN_PUSH && Tok(img, i - 1) == 2 && Tok(img, i - 3) == 0xe1 && Tok(img, i - 5) == SCN_PUSH && Tok(img, i - 4) == 1);
	// lstr(1) = lstr(0) + "_n" + 2: the buffer, three items (the number without a copy), the store
	i = FindTok(img, 0x94, 0, 400);
	CHECK(i < 400 && Tok(img, i - 2) == SCN_LOCAL && Tok(img, i - 1) == 0x100 && Tok(img, i + 1) == SCN_ADD && Tok(img, i + 2) == SCN_STR && Tok(img, i + 4) == 0x48);
	CHECK(Tok(img, i + 10) == SCN_ADD && Tok(img, i + 11) == SCN_PUSH && Tok(img, i + 12) == 2 && Tok(img, i + 13) == SCN_PUSH && Tok(img, i + 14) == 1 && Tok(img, i + 15) == 0xe8);
	// the dispatcher ends the file: str "..not found"; msgbox; quit
	{
		uint32_t codeLen = Tok(img, 4) / 4; // the stream ends where "t.txt" begins
		CHECK(Tok(img, codeLen - 1) == 0xf4 && Tok(img, codeLen - 2) == 0xf9 && Tok(img, codeLen - 4) == SCN_STR);
	}
	BGI_Free(img);

	img = Compile(kScenarioLine, &size, 0);
	CHECK(img != NULL);
	if(!img)
		return;
	base = 0x1c + Rd32(img + 0x1c);
	// the markers are `line` tokens: the source path (with its backslashes) and the line
	CHECK(Tok(img, 3) == SCN_LINE && Tok(img, 5) == 1 && strcmp((const char*)img + base + Tok(img, 4), "D:\\Script\\01.bsb") == 0);
	// msg in the args form: left to right, args 5
	i = FindTok(img, 0x140, 0, 400);
	CHECK(i < 400 && Tok(img, i - 2) == SCN_ARGS && Tok(img, i - 1) == 5 && Tok(img, i - 12) == SCN_STR && strcmp((const char*)img + base + Tok(img, i - 11), "Hello") == 0);
	// the if has a marker; goto = str NAME; set_label; addr 0; jmp
	i = FindTok(img, SCN_JCC, 0, 400);
	CHECK(i < 400 && Tok(img, i - 13) == SCN_LINE && Tok(img, i - 11) == 4);
	CHECK(Tok(img, i + 2) == SCN_LINE && Tok(img, i + 5) == SCN_STR && Tok(img, i + 7) == 0xf6 && Tok(img, i + 8) == SCN_ADDR && Tok(img, i + 9) == 0 && Tok(img, i + 10) == SCN_JMP);
	BGI_Free(img);

	img = Compile(kLibrary, &size, 0);
	CHECK(img != NULL);
	if(!img)
		return;
	{
		ScnFile_t f;
		char err[128];
		CHECK(Scn_Load(img, size, &f, err, sizeof err));
		// main first, then _Second (sorted by upper-cased name)
		CHECK(f.exportCount == 2 && strcmp(f.exports[0].name, "main") == 0 && f.exports[0].off == 0);
		CHECK(strcmp(f.exports[1].name, "_Second") == 0);
		// main's frame: no locals, no parameters, the switch slot: 4
		CHECK(f.code[0] == SCN_PUSHFP && f.code[1] == SCN_PUSH && f.code[2] == 4 && f.code[3] == SCN_ADD && f.code[4] == SCN_SETFP);
		// _Second: locals 4 + 256 + 16 = 276, two parameters: 276 + 8 + 4 = 288; the parameters are popped last first
		{
			uint32_t k = f.exports[1].off / 4;
			CHECK(f.code[k + 2] == 288);
			CHECK(f.code[k + 5] == SCN_LOCAL && f.code[k + 6] == 280 && f.code[k + 7] == SCN_STORE2); // p2 (popped first) at F - 8
			CHECK(f.code[k + 9] == SCN_LOCAL && f.code[k + 10] == 284);                               // p1 at F - 4
			CHECK(f.code[k + 13] == SCN_ADDR && f.code[k + 15] == SCN_TRY);
			// the first statement: line "Lib.bsc", 3 (the line after "#line 1" is line 1)
			CHECK(f.code[k + 16] == SCN_LINE && f.code[k + 18] == 3);
		}
		// the string table: the source name, the include, then the strings
		CHECK(strcmp((const char*)f.strings, "Lib.bsc") == 0 && strcmp((const char*)f.strings + 8, "bss.h") == 0);
		Scn_Free(&f);
	}
	BGI_Free(img);
}

static void TestRoundTrip(void)
{
	const char* sources[3];
	int s;
	sources[0] = kScenario;
	sources[1] = kLibrary;
	sources[2] = kScenarioLine;
	for(s = 0; s < 3; s++)
	{
		uint32_t size, size2;
		uint8_t* img = Compile(sources[s], &size, 0);
		char* text;
		uint8_t* img2;
		int fallbacks = -1;
		CHECK(img != NULL);
		if(!img)
			continue;
		text = Decompile(img, size, &fallbacks);
		CHECK(text != NULL && fallbacks == 0);
		if(!text)
		{
			BGI_Free(img);
			continue;
		}
		img2 = Compile(text, &size2, 0);
		CHECK(img2 != NULL && size2 == size && memcmp(img, img2, size) == 0);
		if(s == 0)
		{
			CHECK(strstr(text, "msg(\"Hello\", \"Name\", 0, 1, 1);") != NULL);
			CHECK(strstr(text, "jump \"_02\", \"top\";") != NULL);
			CHECK(strstr(text, "goto start;") != NULL && strstr(text, "\nstart:") != NULL);
			CHECK(strstr(text, "call sub;") != NULL);
			CHECK(strstr(text, "lstr(0) = \"bg01\";") != NULL);
			CHECK(strstr(text, "lstr(1) = lstr(0) + \"_n\" + 2;") != NULL);
			CHECK(strstr(text, "\n;\n") != NULL);
			CHECK(strstr(text, "set_lwork!(2, lwork!(1));") != NULL);
			CHECK(strstr(text, "} else {") != NULL);
			CHECK(strstr(text, "5 + -3") != NULL);
		}
		else if(s == 2)
		{
			CHECK(strstr(text, "#style scenario-line") != NULL);
			CHECK(strstr(text, "#source \"D:\\\\Script\\\\01.bsb\"") != NULL);
			CHECK(strstr(text, "msg(\"Hello\", \"Name\", 0, 1, 1);") != NULL);
			CHECK(strstr(text, "} else if (lwork(0) == 2) {") != NULL);
			CHECK(strstr(text, "goto next;") != NULL && strstr(text, "\nnext:") != NULL);
		}
		else
		{
			CHECK(strstr(text, "while (a < 4) {") != NULL || strstr(text, "while (l4 < 4) {") != NULL);
			CHECK(strstr(text, "switch (p1) {") != NULL && strstr(text, "default:") != NULL && strstr(text, "case 1:") != NULL);
			CHECK(strstr(text, "if (lwork(2)) {") != NULL);
			CHECK(strstr(text, "char* p2") != NULL && strstr(text, "p2[0] == 95") != NULL);
			CHECK(strstr(text, "return -") != NULL);
		}
		free(text);
		BGI_Free(img);
		BGI_Free(img2);
	}
}

// the headerless variant of the 1.66 / 1.69 games: no container, calls by address, byte parameters, #file and #string
static const char* kRaw = "#style library-raw\n"
						  "#source \"main.bss\"\n"
						  "#include \"sub.h\"\n"
						  "#string \"hi\"\n"
						  "#line 1\n"
						  "#file \"sub.h\"\n"
						  "#line 3\n"
						  "fn_0001(p1, char p2) { char l100[256];\n"
						  "    strcpy(l100, \"x\");\n"
						  "    voice(l100);\n"
						  "}\n"
						  "#file \"main.bss\"\n"
						  "#line 10\n"
						  "main() {\n"
						  "    fn_0001(1, 2);\n"
						  "    @0x1234(3, 4);\n"
						  "    @0x1240(5);\n"
						  "    msg(\"hi\", \"n\", 0, 1, 1);\n"
						  "}\n";

static void TestRaw(void)
{
	uint32_t size, size2, i, n;
	uint8_t* img = Compile(kRaw, &size, 0);
	ScnFile_t f;
	char err[128];
	char* text;
	uint8_t* img2;
	int fallbacks = -1;
	CHECK(img != NULL);
	if(!img)
		return;
	// no header: the frame prologue of main lies at offset 0; the strings follow the code
	CHECK(Rd32(img) == SCN_PUSHFP && Rd32(img + 4) == SCN_PUSH && Rd32(img + 12) == SCN_ADD && Rd32(img + 16) == SCN_SETFP);
	CHECK(Scn_Load(img, size, &f, err, sizeof err));
	CHECK(f.raw == 1 && f.image == NULL && f.exportCount == 0 && f.base == 0);
	n = f.codeLen;
	// `@0x1234(3, 4)`: the arguments left to right (no `args`), the address, the call-by-address token
	for(i = 0; i + 6 < n; i++)
		if(f.code[i] == SCN_PUSH && f.code[i + 1] == 3 && f.code[i + 2] == SCN_PUSH && f.code[i + 3] == 4)
			break;
	CHECK(i + 6 < n && f.code[i + 4] == SCN_PUSH && f.code[i + 5] == 0x1234 && f.code[i + 6] == SCN_RAWCALL);
	for(i = 0; i + 4 < n; i++)
		if(f.code[i] == SCN_PUSH && f.code[i + 1] == 5 && f.code[i + 2] == SCN_PUSH && f.code[i + 3] == 0x1240)
			break;
	CHECK(i + 4 < n && f.code[i + 4] == SCN_RAWCALL);
	// the string table: the source name, the include, then the #string literal before the others
	CHECK(f.stringsLen > 0 && strcmp(Scn_String(&f, f.codeLen * 4), "main.bss") == 0);
	CHECK(strcmp(Scn_String(&f, f.codeLen * 4 + 9), "sub.h") == 0);
	CHECK(strcmp(Scn_String(&f, f.codeLen * 4 + 15), "hi") == 0);
	CHECK(strcmp(Scn_String(&f, f.codeLen * 4 + 18), "x") == 0);
	// the functions found without exports: main at 0 and fn_0001 after its ret, with their parameter counts
	{
		ScnFuncInfo_t* list = NULL;
		int count = 0, k, foundMain = 0, foundSub = 0;
		Scn_ListFunctions(&f, &list, &count);
		CHECK(count == 2);
		for(k = 0; k < count; k++)
		{
			if(strcmp(list[k].name, "@0x0") == 0 && list[k].params == 0)
				foundMain = 1;
			if(list[k].name[0] == '@' && list[k].params == 2)
				foundSub = 1;
		}
		CHECK(foundMain && foundSub);
		free(list);
	}
	Scn_Free(&f);
	// the round trip, and the shapes in the decompiled text
	text = Decompile(img, size, &fallbacks);
	CHECK(text != NULL && fallbacks == 0);
	if(text)
	{
		img2 = Compile(text, &size2, 0);
		CHECK(img2 != NULL && size2 == size && memcmp(img, img2, size) == 0);
		CHECK(strstr(text, "#style library-raw") != NULL);
		CHECK(strstr(text, "#string \"hi\"") != NULL);
		CHECK(strstr(text, "#file \"sub.h\"") != NULL);
		CHECK(strstr(text, "char p2") != NULL);
		CHECK(strstr(text, "@0x1234(3, 4);") != NULL && strstr(text, "@0x1240(5);") != NULL);
		CHECK(strstr(text, "msg(\"hi\", \"n\", 0, 1, 1);") != NULL);
		free(text);
		BGI_Free(img2);
	}
	BGI_Free(img);
}

// a 16-bit scene of the 1.64 games
static const char* kScene16 = "#style inline\n"
							  "\n"
							  "sys_db(1);\n"
							  "lwork(1) = 0;\n"
							  "bg(\"bg01\", 1000);\n"
							  "kana(\"name\", \"reading\");\n"
							  "msg(0, 1, \"Hello\");\n"
							  "name(\"N\");\n"
							  "top:\n"
							  "if (lwork(1) < 5) goto done;\n"
							  "if (!flag(3)) goto top;\n"
							  "lwork(1) += 1;\n"
							  "gwork(2) = lwork(1);\n"
							  "goto top;\n"
							  "done:\n"
							  "select(\"a\", \"b\");\n"
							  "select_jump(top, done);\n"
							  "call top;\n"
							  "call \"font\";\n"
							  "sprite_move(1, 0x80000010, -2, 0, 0, 500);\n"
							  "jump \"next\";\n"
							  "return;\n"
							  "end;\n";

static void TestScene16(void)
{
	uint32_t size, size2, i;
	uint8_t* img = Compile(kScene16, &size, 0);
	ScnFile_t f;
	char err[128];
	char* text;
	uint8_t* img2;
	int fallbacks = -1;
	CHECK(img != NULL);
	if(!img)
		return;
	// the first instructions: `db 1`, `98 lwork(1) 0`, `28 "bg01" 1000`
	CHECK(size > 0x30 && img[0] == 0xdb && img[1] == 0 && Rd32(img + 2) == 1);
	CHECK(img[6] == 0x98 && Rd32(img + 8) == 0x10001 && Rd32(img + 12) == 0);
	CHECK(img[16] == 0x28 && memcmp(img + 18, "bg01", 5) == 0 && Rd32(img + 23) == 1000);
	// the message: its text lies at the first multiple of 16 after the code, the padding is zero
	{
		uint32_t msgAt = 27 + 2 + 5 + 8; // after kana("name", "reading")
		uint32_t textAt;
		CHECK(img[msgAt] == 0x10 && Rd32(img + msgAt + 2) == 0 && Rd32(img + msgAt + 6) == 1);
		textAt = Rd32(img + msgAt + 10);
		CHECK((textAt & 15) == 0 && textAt < size && strcmp((const char*)img + textAt, "Hello") == 0);
		for(i = textAt - 16; i < textAt; i++)
			if(img[i] == 0xc2 && img[i + 1] == 0) // the `end` is the last instruction
				break;
		CHECK(i < textAt);
		for(i += 2; i < textAt; i++)
			CHECK(img[i] == 0);
		CHECK(textAt + 6 == size); // the one text ends the file
	}
	// the jumps: `a8 lwork(1) 5 L`, `a2 3 L`, `99 lwork(1) 1`, `98 gwork(2) lwork(1)`, `a0 L`
	{
		uint32_t top = 27 + 2 + 5 + 8 + 14 + 2 + 2; // the label after name("N")
		uint32_t p = top;
		CHECK(img[p] == 0xa8 && Rd32(img + p + 2) == 0x10001 && Rd32(img + p + 6) == 5);
		p += 14;
		CHECK(img[p] == 0xa2 && Rd32(img + p + 2) == 3 && Rd32(img + p + 6) == top);
		p += 10;
		CHECK(img[p] == 0x99 && Rd32(img + p + 2) == 0x10001 && Rd32(img + p + 6) == 1);
		p += 10;
		CHECK(img[p] == 0x98 && Rd32(img + p + 2) == 0x20002 && Rd32(img + p + 6) == 0x10001);
		p += 10;
		CHECK(img[p] == 0xa0 && Rd32(img + p + 2) == top);
		p += 6;
		CHECK(Rd32(img + top + 10) == p); // `done`
		CHECK(img[p] == 0xb0 && Rd32(img + p + 2) == 2 && memcmp(img + p + 6, "a\0b\0", 4) == 0);
		p += 10;
		CHECK(img[p] == 0xa9 && Rd32(img + p + 2) == 2 && Rd32(img + p + 6) == top && Rd32(img + p + 10) == p - 10);
		p += 14;
		CHECK(img[p] == 0xac && Rd32(img + p + 2) == top);
		p += 6;
		CHECK(img[p] == 0xc1 && memcmp(img + p + 2, "font", 5) == 0);
		p += 7;
		CHECK(img[p] == 0x36 && Rd32(img + p + 6) == 0x80000010 && Rd32(img + p + 10) == 0xfffffffe);
	}
	// it loads as an image and saves back
	CHECK(Scn_Load(img, size, &f, err, sizeof err));
	CHECK(f.image != NULL && f.imageSize == size && f.code == NULL);
	img2 = Scn_Save(&f, &size2);
	CHECK(size2 == size && memcmp(img2, img, size) == 0);
	BGI_Free(img2);
	Scn_Free(&f);
	// the round trip and the spellings
	text = Decompile(img, size, &fallbacks);
	CHECK(text != NULL && fallbacks == 0);
	if(text)
	{
		img2 = Compile(text, &size2, 0);
		CHECK(img2 != NULL && size2 == size && memcmp(img, img2, size) == 0);
		CHECK(strstr(text, "#style inline") != NULL);
		CHECK(strstr(text, "msg(0, 1, \"Hello\");") != NULL);
		CHECK(strstr(text, "if (lwork(1) < 5) goto L_") != NULL);
		CHECK(strstr(text, "if (!flag(3)) goto L_") != NULL);
		CHECK(strstr(text, "lwork(1) += 1;") != NULL);
		CHECK(strstr(text, "gwork(2) = lwork(1);") != NULL);
		CHECK(strstr(text, "select(\"a\", \"b\");") != NULL);
		CHECK(strstr(text, "select_jump(L_") != NULL);
		CHECK(strstr(text, "call \"font\";") != NULL && strstr(text, "jump \"next\";") != NULL);
		CHECK(strstr(text, "sprite_move(1, 0x80000010, -2, 0, 0, 500);") != NULL);
		CHECK(strstr(text, "\nreturn;\nend;\n") != NULL);
		free(text);
		BGI_Free(img2);
	}
	BGI_Free(img);
	// a scene without a message has no padding; one with no instruction is not a scene
	img = Compile("#style inline\nwait(5);\nend;\n", &size, 0);
	CHECK(img != NULL && size == 8);
	if(img)
	{
		CHECK(Scn_Load(img, size, &f, err, sizeof err) && f.image != NULL);
		Scn_Free(&f);
		BGI_Free(img);
	}
	{
		static const uint8_t bad[] = {0x10, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0xff, 0xff, 0xff, 0xff};
		static const uint8_t pad[] = {0xc2, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; // non-zero padding
		CHECK(!Scn_Load(bad, sizeof bad, &f, err, sizeof err));                           // a text offset out of range
		CHECK(!Scn_Load(pad, sizeof pad, &f, err, sizeof err));
	}
	// errors
	CHECK(Compile("#style inline\nnothing(1);\n", &size, 1) == NULL);         // an unknown command
	CHECK(Compile("#style inline\ngoto nowhere;\n", &size, 1) == NULL);       // an undefined label
	CHECK(Compile("#style inline\nselect();\n", &size, 1) == NULL);           // an empty list
	CHECK(Compile("#style inline\nmsg(0, 1);\n", &size, 1) == NULL);          // an argument missing
	CHECK(Compile("#style inline\nbg(1, 2);\n", &size, 1) == NULL);           // a string expected
	CHECK(Compile("#style inline\n#source \"x\"\nend;\n", &size, 1) == NULL); // no other directive
}

// the reader of PrepIncludes_t for the one file the scene below includes
static uint8_t* ReadStrings(void* ctx, const char* path, uint32_t* size)
{
	static const char text[] = "// the texts\n"
							   "#define NAME_N  \"N\"\n"
							   "#define T_HELLO \"Hello\"  // N\n"
							   "#define CLICK   1\n";
	uint8_t* copy;
	(void)ctx;
	if(strcmp(path, "strings.inc") != 0)
		return NULL;
	copy = (uint8_t*)malloc(sizeof text);
	memcpy(copy, text, sizeof text);
	*size = (uint32_t)(sizeof text - 1);
	return copy;
}

/* a 16-bit scene goes through the preprocessor: the constants of an
 * included file and of the scene itself give the bytes of the literals */
static void TestScene16Prep(void)
{
	static const char named[] = "#style inline\n"
								"#include \"strings.inc\"\n"
								"#define SPEED 500\n"
								"msg(0, CLICK, T_HELLO);\n"
								"name(NAME_N);\n"
								"msg(0, 0, T_HELLO); // T_HELLO again: its own text\n"
								"select(T_HELLO, NAME_N);\n"
								"wait(SPEED);\n"
								"end;\n";
	static const char plain[] = "#style inline\n"
								"msg(0, 1, \"Hello\");\n"
								"name(\"N\");\n"
								"msg(0, 0, \"Hello\");\n"
								"select(\"Hello\", \"N\");\n"
								"wait(500);\n"
								"end;\n";
	PrepIncludes_t inc;
	ScnCompileResult_t r;
	uint32_t size = 0, size2 = 0;
	uint8_t* a;
	uint8_t* b = Compile(plain, &size2, 0);
	memset(&inc, 0, sizeof inc);
	inc.read = ReadStrings;
	Scn_Compile(named, strlen(named), "scene.bsc", &gCmds, &inc, stderr, &r);
	CHECK(r.errors == 0);
	a = r.errors ? NULL : Scn_Save(&r.file, &size);
	if(!r.errors)
		Scn_Free(&r.file);
	CHECK(a != NULL && b != NULL && size == size2 && memcmp(a, b, size) == 0);
	BGI_Free(a);
	BGI_Free(b);
	CHECK(Compile("#style inline\nmsg(0, 1, T_NONE);\nend;\n", &size, 1) == NULL);                 // a name nothing defines
	CHECK(Compile("#style inline\n#include \"no/such.inc\"\nend;\n", &size, 1) == NULL);           // a file that is not there
	CHECK(Compile("#style inline\n#define A 1\n#define A 2\nwait(A);\nend;\n", &size, 1) == NULL); // defined twice
	{                                                                                              // the other styles are not preprocessed: their #include names a header of the original source
		uint8_t* img = Compile("#style library\n#include \"header.h\"\nf() { return; }\n", &size, 0);
		CHECK(img != NULL);
		BGI_Free(img);
	}
}

static void TestErrors(void)
{
	uint32_t size;
	CHECK(Compile("#style library\nf() { x = 1; }\n", &size, 1) == NULL);                     // an unknown variable
	CHECK(Compile("#style scenario\nbreak;\n", &size, 1) == NULL);                            // a break outside a loop
	CHECK(Compile("#style library\nf() { int a; a = ; }\n", &size, 1) == NULL);               // a syntax error
	CHECK(Compile("#style scenario\nmsg(1\n", &size, 1) == NULL);                             // unterminated
	CHECK(Compile("#style library\nf() { goto nowhere; }\n", &size, 1) == NULL);              // an undefined label
	CHECK(Compile("#style library\nf() { if (1) { } else if (2) { } }\n", &size, 1) == NULL); // no else-if form with markers
	{
		uint8_t* img = Compile("#style scenario\n_F(1);\n", &size, 1); // no #source: no markers at all
		CHECK(img != NULL && Tok(img, 3) == SCN_PUSH);
		BGI_Free(img);
	}
}

int main(void)
{
	if(!OS_Init())
		return 1;
	printf("container\n");
	TestContainer();
	printf("compile\n");
	TestCompile();
	printf("round trip\n");
	TestRoundTrip();
	printf("raw\n");
	TestRaw();
	printf("16-bit scenes\n");
	TestScene16();
	TestScene16Prep();
	printf("errors\n");
	TestErrors();
	OS_Shutdown();
	if(gFails)
	{
		printf("%d check(s) failed\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
