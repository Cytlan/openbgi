/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bgipack.c - build a game's archives again from a dump made by bgidump
 *             (docs/modding/dump-pack.md): `openbgi pack`, or the program by the name
 *             bgipack (tools.h)
 *
 *   openbgi pack [options] DUMPDIR OUTDIR
 *
 * Reads DUMPDIR/bgidump.json and writes every archive it lists into
 * OUTDIR.  An entry whose files are as bgidump wrote them is copied from
 * the original archive; an entry whose files were edited is compiled or
 * encoded again.  The original archives are read from the directory the
 * dump was made from (or --source) and are never written to.
 *
 *   -s, --source DIR    the directory of the original archives
 *       --only NAME     build this archive only (may be given several times)
 *   -c, --changed       write only the archives with a changed entry
 *   -a, --rebuild-all   build every entry from its files; the originals
 *                       are not read
 *       --checksum      compare the files by CRC, not by size and time
 *   -v                  a line per entry built
 *   -q                  problems and the summary only
 *
 * The exit status is 0 on success, 1 when an archive was written with an
 * original entry in place of one that did not build, 2 when an archive
 * could not be written.
 */
#include "tools/dump.h"
#include "tools/tools.h"
#include "bgi/core.h"
#include "host/os.h"
#include "bgi/release.h"

// the options; asked for (--help) on standard output with the status 0, else on stderr with the status 2
static void Usage(const char* prog, int asked)
{
	FILE* out = asked ? stdout : stderr;
	fprintf(out, "usage: %s [options] DUMPDIR OUTDIR\n", prog);
	fputs("The archives of a game again, into OUTDIR, from the dump in DUMPDIR (docs/modding/dump-pack.md).\n"
		  "  -s, --source DIR    the directory of the original archives (default: where the dump was made from)\n"
		  "      --only NAME     build this archive only (repeatable)\n"
		  "  -c, --changed       write only the archives with a changed entry\n"
		  "  -a, --rebuild-all   build every entry from its files; the originals are not read\n"
		  "      --checksum      compare the files by CRC, not by size and time\n"
		  "  -v                  a line per entry built\n"
		  "  -q                  problems and the summary only\n"
		  "      --version       print the OpenBGI version and exit\n"
		  "  -h, --help          print this and exit\n",
		out);
	exit(asked ? 0 : 2);
}

int Tool_Pack(int argc, char** argv)
{
	const char* prog = argv[0];
	PackOptions_t opt;
	const char* only[256];
	const char* dirs[2];
	int dirCount = 0, i;
	OpenBGI_t* o = OpenBGI_New(OS_Host()); // the engine whose code the tool runs
	if(!o)
		return 1;
	for(i = 1; i < argc; i++)
	{
		if(strcmp(argv[i], "--version") == 0)
		{
			printf("%s (%s)\n", prog, OPENBGI_TITLE);
			return 0;
		}
		if(strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
			Usage(prog, 1);
	}
	memset(&opt, 0, sizeof opt);
	opt.verbose = 1;
	opt.only = only;
	for(i = 1; i < argc; i++)
	{
		const char* a = argv[i];
		const char* value = i + 1 < argc ? argv[i + 1] : NULL;
		if((strcmp(a, "-s") == 0 || strcmp(a, "--source") == 0) && value)
			opt.sourceDir = argv[++i];
		else if(strncmp(a, "--source=", 9) == 0)
			opt.sourceDir = a + 9;
		else if(strcmp(a, "--only") == 0 && value && opt.onlyCount < (int)BGI_COUNTOF(only))
			only[opt.onlyCount++] = argv[++i];
		else if(strcmp(a, "-c") == 0 || strcmp(a, "--changed") == 0)
			opt.changedOnly = 1;
		else if(strcmp(a, "-a") == 0 || strcmp(a, "--rebuild-all") == 0)
			opt.rebuildAll = 1;
		else if(strcmp(a, "--checksum") == 0)
			opt.checksum = 1;
		else if(strcmp(a, "-v") == 0)
			opt.verbose = 2;
		else if(strcmp(a, "-q") == 0)
			opt.verbose = 0;
		else if(a[0] == '-' && a[1])
			Usage(prog, 0);
		else if(dirCount < 2)
			dirs[dirCount++] = a;
		else
			Usage(prog, 0);
	}
	if(dirCount != 2)
		Usage(prog, 0);
	opt.dumpDir = dirs[0];
	opt.outDir = dirs[1];
	if(!OS_Init()) // the Shift-JIS tables
		return 2;
	return Pack_Run(o, &opt);
}
