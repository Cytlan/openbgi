/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/tools.c - unit test of the tools as commands of the program
 *                 (src/tools/tools.c, src/tools/tools.h); built as
 *                 test_tools by `make test` in a build that has them
 *
 * The groups of tests, in the order they run:
 *
 *   engine's - starts that are no tool's: no arguments, a game, options,
 *              a folder spelled "./dump", a program whose name only
 *              begins like a tool's
 *   names    - the program under a tool's name, with what a build puts
 *              after it and with a folder before it
 *   commands - a source compiled with `bpc`, the same with bpasm's -C by
 *              the program's name (the two files are the same), and the
 *              program decompiled again with `asm -D`
 *
 * The files are written into a scratch directory of the run.
 */
#include "tools/tools.h"

#include "check.h"
#include "testhost.h"

static char gRoot[0x300];

static const char kSource[] = "#engine 1.553\n"
							  "helper() { return *(int*)1128 + *(int*)1132; }\n"
							  "main() { *(int*)1124 = helper() + *(int*)1136; }\n";

// Tools_Run on the arguments given (ended by NULL): -1 when the start is the engine's, else the tool's status
static int Start(const char* arg0, ...)
{
	char* argv[16];
	int argc = 0, status = -2;
	va_list ap;
	va_start(ap, arg0);
	for(const char* a = arg0; a && argc < 15; a = va_arg(ap, const char*))
		argv[argc++] = (char*)a;
	va_end(ap);
	argv[argc] = NULL;
	return Tools_Run(argc, argv, &status) ? status : -1;
}

// the path of a file in the scratch directory (a few at a time)
static const char* In(const char* name)
{
	static char path[8][0x400];
	static int next;
	char* p = path[next++ & 7];
	snprintf(p, sizeof path[0], "%s/%s", gRoot, name);
	return p;
}

// a whole file (free() it), NULL when there is none
static char* Slurp(const char* path, long* size)
{
	FILE* f = fopen(path, "rb");
	char* data;
	if(!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = (char*)malloc((size_t)*size + 1);
	if(data && fread(data, 1, (size_t)*size, f) != (size_t)*size)
		*size = 0;
	if(data)
		data[*size] = 0;
	fclose(f);
	return data;
}

static void TestEngines(void)
{
	CHECK(Start("openbgi", NULL) == -1);
	CHECK(Start("/opt/openbgi/openbgi-x86_64", "/games/one", NULL) == -1);
	CHECK(Start("openbgi", "--engine=1.553", "game.zip", NULL) == -1);
	CHECK(Start("openbgi", "./dump", NULL) == -1);
	CHECK(Start("openbgi", "bpc.zip", NULL) == -1);
	CHECK(Start("bpcplayer", "--version", NULL) == -1);
	CHECK(Start("dump", "--version", NULL) == -1); // (a command is an argument, not a name)
}

static void TestNames(void)
{
	CHECK(Start("bgidump", "--version", NULL) == 0);
	CHECK(Start("bin/linux/bgipack-x86_64", "--version", NULL) == 0);
	CHECK(Start("C:\\Program Files\\OpenBGI\\bgiscn-x86.exe", "--version", NULL) == 0);
	CHECK(Start("./bpasm", "--version", NULL) == 0);
	CHECK(Start("bpc.exe", "--version", NULL) == 0);
	CHECK(Start("openbgi", "dump", "--version", NULL) == 0);
	CHECK(Start("openbgi.com", "pack", "--version", NULL) == 0);
}

static void TestCommands(void)
{
	const char* src = In("one.bpc");
	const char* a = In("a._bp");
	const char* b = In("b._bp");
	const char* text = In("a.bpc");
	long an = 0, bn = 0, tn = 0;
	char *ad, *bd, *td;
	FILE* f = fopen(src, "wb");
	CHECK(f != NULL);
	if(!f)
		return;
	fputs(kSource, f);
	fclose(f);

	CHECK(Start("openbgi", "bpc", src, "-o", a, NULL) == 0);
	CHECK(Start("bpasm-x86_64", "-C", src, "-o", b, NULL) == 0);
	ad = Slurp(a, &an);
	bd = Slurp(b, &bn);
	CHECK(ad && bd && an > 16 && an == bn && memcmp(ad, bd, (size_t)an) == 0);
	CHECK(ad && ad[0] == 0x10 && ad[1] == 0); // a program file: its header's size first
	free(ad);
	free(bd);

	CHECK(Start("openbgi", "asm", "-D", "-e", "1.553", a, "-o", text, NULL) == 0);
	td = Slurp(text, &tn);
	CHECK(td && strstr(td, "#engine 1.553") && strstr(td, "*(int*)1124 = "));
	free(td);

	// a source that does not compile: the status says so, and there is no file
	f = fopen(src, "wb");
	if(f)
	{
		fputs("main() { return nothing; }\n", f);
		fclose(f);
	}
	CHECK(Start("openbgi", "bpc", src, "-o", In("c._bp"), NULL) == 1);
	CHECK(Slurp(In("c._bp"), &an) == NULL);
	// a source that is not there
	CHECK(Start("openbgi", "bpc", In("none.bpc"), "-o", In("c._bp"), NULL) == 1);
}

int main(void)
{
	TestHost_ScratchName(gRoot, sizeof gRoot, "bgi_tools");
	if(TestHost_Mkdir(gRoot) != 0)
	{
		printf("tools: cannot make %s\n", gRoot);
		return 1;
	}
	printf("engine's\n");
	TestEngines();
	printf("names\n");
	TestNames();
	printf("commands\n");
	TestCommands();
	fflush(stdout);
	if(gFails)
	{
		printf("tools: %d checks failed (the files are kept in %s)\n", gFails, gRoot);
		return 1;
	}
	if(OS_Init())
		TestHost_RemoveTree(gRoot);
	printf("tools: all tests passed\n");
	return 0;
}
