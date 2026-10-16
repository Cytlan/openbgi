/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bgidump.c - dump every archive of a game into editable files
 *             (docs/dump.md); `make tools` builds bin/bgidump
 *
 *   bgidump [options] GAMEDIR OUTDIR
 *
 * Every "*.arc" of GAMEDIR gets a directory under OUTDIR: programs and
 * scenario files decompiled, images as PNG, sounds as Ogg Vorbis, frame
 * sequences as PNG frames, tables as JSON, with a JSON file beside an
 * asset for what its new format cannot hold.  OUTDIR/bgidump.json
 * describes the dump for bgipack, which builds the archives again.
 *
 *   -e, --engine NAME   the engine profile (default: found through games.json
 *                       from the game's ipl._bp; "any" for none)
 *   -g, --games FILE    the games.json to identify the game with
 *       --only NAME     dump this archive only (may be given several times)
 *   -j N                worker processes (default: one per processor)
 *       --png-level N   PNG compression 1 (fast) .. 9 (small), default 4
 *   -f, --force         write into a directory that already holds a dump
 *   -v                  a line per entry
 *   -q                  problems and the summary only
 *
 * The exit status is 0 when everything converted, 1 when the dump is
 * complete but an entry is noted as a problem, 2 when there is no dump.
 */
#include "bgi/dump.h"
#include "bgi/os.h"

static void Usage(void)
{
	fputs("usage: bgidump [options] GAMEDIR OUTDIR\n"
		  "  -e, --engine NAME   engine profile (default: from games.json and the game's ipl._bp)\n"
		  "  -g, --games FILE    the games.json to identify the game with\n"
		  "      --only NAME     dump this archive only (repeatable)\n"
		  "  -j N                worker processes (default: one per processor)\n"
		  "      --png-level N   PNG compression, 1 (fast) .. 9 (small); default 4\n"
		  "  -f, --force         write into a directory that already holds a dump\n"
		  "  -v                  a line per entry\n"
		  "  -q                  problems and the summary only\n",
		stderr);
	exit(2);
}

int main(int argc, char** argv)
{
	DumpOptions_t opt;
	const char* only[256];
	const char* dirs[2];
	int dirCount = 0, i;
	OS_NativeArgs(&argc, &argv);
	memset(&opt, 0, sizeof opt);
	opt.verbose = 1;
	opt.only = only;
	for(i = 1; i < argc; i++)
	{
		const char* a = argv[i];
		const char* value = i + 1 < argc ? argv[i + 1] : NULL;
		if((strcmp(a, "-e") == 0 || strcmp(a, "--engine") == 0) && value)
			opt.engine = argv[++i];
		else if(strncmp(a, "--engine=", 9) == 0)
			opt.engine = a + 9;
		else if((strcmp(a, "-g") == 0 || strcmp(a, "--games") == 0) && value)
			opt.gamesJson = argv[++i];
		else if(strncmp(a, "--games=", 8) == 0)
			opt.gamesJson = a + 8;
		else if(strcmp(a, "--only") == 0 && value && opt.onlyCount < (int)BGI_COUNTOF(only))
			only[opt.onlyCount++] = argv[++i];
		else if(strcmp(a, "-j") == 0 && value)
			opt.jobs = atoi(argv[++i]);
		else if(strncmp(a, "-j", 2) == 0 && a[2] >= '0' && a[2] <= '9')
			opt.jobs = atoi(a + 2);
		else if(strcmp(a, "--png-level") == 0 && value)
			opt.pngLevel = atoi(argv[++i]);
		else if(strcmp(a, "-f") == 0 || strcmp(a, "--force") == 0)
			opt.force = 1;
		else if(strcmp(a, "-v") == 0)
			opt.verbose = 2;
		else if(strcmp(a, "-q") == 0)
			opt.verbose = 0;
		else if(a[0] == '-' && a[1])
			Usage();
		else if(dirCount < 2)
			dirs[dirCount++] = a;
		else
			Usage();
	}
	if(dirCount != 2)
		Usage();
	opt.gameDir = dirs[0];
	opt.outDir = dirs[1];
	OS_SetArgv0(argv[0]);
	if(!OS_Init()) // the Shift-JIS tables
		return 2;
	return Dump_Run(&opt);
}
