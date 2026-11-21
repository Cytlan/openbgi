/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tools.c - which tool a start of the program is for, and the help
 *           (tools.h, docs/modding/README.md)
 */
#include "tools/tools.h"
#include "bgi/engine.h"
#include "bgi/release.h"
#include "host/os.h"

#include <stdlib.h>
#include <string.h>

typedef struct Tool
{
	const char* command;               // openbgi COMMAND
	const char* program;               // its name as a program of its own
	int (*run)(int argc, char** argv); // the tool
	int nativeArgs;                    // its arguments are paths of the OS layer (OS_NativeArgs)
	const char* summary;               // a line of `openbgi help`
} Tool_t;

static const Tool_t kTools[] = {
	{"dump", "bgidump", Tool_Dump, 1, "every archive of a game into files to edit"},
	{"pack", "bgipack", Tool_Pack, 1, "the archives again, from a dump"},
	{"scn", "bgiscn", Tool_Scn, 0, "scenario files: decompile, compile, list"},
	{"asm", "bpasm", Tool_Asm, 0, "programs as listings: disassemble, assemble"},
	{"bpc", "bpc", Tool_Bpc, 0, "programs in the source language: decompile, compile"},
};
enum
{
	TOOL_COUNT = sizeof kTools / sizeof kTools[0]
};

void Tools_Usage(FILE* out, const char* prog, const char* const* lines)
{
	for(int i = 0; lines[i]; i++)
		fprintf(out, "%s %s %s\n", i ? "      " : "usage:", prog, lines[i]);
}

/* the tool a program of the name `argv0` is: the file's name is a tool's,
 * alone or with something after a '-' or a '.' ("bpasm", "bpasm-x86_64",
 * "bpasm.exe"); NULL for any other name */
static const Tool_t* ToolOfProgram(const char* argv0)
{
	const char* name = argv0;
	for(const char* p = argv0; *p; p++)
		if(*p == '/' || *p == '\\')
			name = p + 1;
	for(int i = 0; i < TOOL_COUNT; i++)
	{
		size_t n = strlen(kTools[i].program);
		if(strncmp(name, kTools[i].program, n) == 0 && (!name[n] || name[n] == '-' || name[n] == '.'))
			return &kTools[i];
	}
	return NULL;
}

static const Tool_t* ToolOfCommand(const char* word)
{
	for(int i = 0; i < TOOL_COUNT; i++)
		if(strcmp(word, kTools[i].command) == 0)
			return &kTools[i];
	return NULL;
}

// `openbgi help`: the engine's options, then the commands
static void Help(void)
{
	printf("%s (https://openbgi.net)\n\n", OPENBGI_TITLE);
	Engine_PrintUsage(stdout);
	printf("\nusage: openbgi COMMAND [arguments]\n\n");
	for(int i = 0; i < TOOL_COUNT; i++)
		printf("  %-6s %s\n", kTools[i].command, kTools[i].summary);
	printf("  %-6s this; `openbgi help COMMAND` or `openbgi COMMAND --help` for a command's arguments\n", "help");
	printf("\nA folder named like a command is given as ./NAME.\n");
}

// the tool run on argv[first ..], as `shown` in its messages
static int RunTool(const Tool_t* t, const char* shown, int argc, char** argv, int first)
{
	char name[64];
	char** av;
	int status, n = 0;
	if(t->nativeArgs)
	{ // the same arguments, spelled as the OS layer's files want them: the process's own, so only when these are
		int nativeCount = argc;
		char** native = argv;
		OS_NativeArgs(&nativeCount, &native);
		if(nativeCount == argc)
			argv = native;
	}
	av = (char**)calloc((size_t)argc + 2, sizeof *av);
	if(!av)
		return 1;
	snprintf(name, sizeof name, "%s", shown);
	av[n++] = name;
	for(int i = first; i < argc; i++)
		av[n++] = argv[i];
	status = t->run(n, av);
	free(av);
	return status;
}

int Tools_Run(int argc, char** argv, int* status)
{
	char shown[64];
	const Tool_t* t;
	if(argc < 1 || !argv || !argv[0])
		return 0;
	t = ToolOfProgram(argv[0]);
	if(t)
	{
		*status = RunTool(t, t->program, argc, argv, 1);
		return 1;
	}
	if(argc < 2)
		return 0;
	if(strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)
	{
		char help[] = "--help";
		char* av[2] = {shown, help};
		t = argc >= 3 && strcmp(argv[1], "help") == 0 ? ToolOfCommand(argv[2]) : NULL;
		if(!t)
		{
			if(argc >= 3 && strcmp(argv[1], "help") == 0)
			{
				fprintf(stderr, "openbgi: no command %s\n", argv[2]);
				*status = 2;
				return 1;
			}
			Help();
			*status = 0;
			return 1;
		}
		snprintf(shown, sizeof shown, "openbgi %s", t->command);
		*status = t->run(2, av);
		return 1;
	}
	t = ToolOfCommand(argv[1]);
	if(!t)
		return 0;
	snprintf(shown, sizeof shown, "openbgi %s", t->command);
	*status = RunTool(t, shown, argc, argv, 2);
	return 1;
}
