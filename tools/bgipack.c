/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bgipack.c - build a game's archives again from a dump made by bgidump
 *             (docs/dump.md); `make tools` builds bin/bgipack
 *
 *   bgipack [options] DUMPDIR OUTDIR
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
#include "bgi/dump.h"
#include "bgi/os.h"

static void Usage(void)
{
	fputs("usage: bgipack [options] DUMPDIR OUTDIR\n"
		  "  -s, --source DIR    the directory of the original archives (default: where the dump was made from)\n"
		  "      --only NAME     build this archive only (repeatable)\n"
		  "  -c, --changed       write only the archives with a changed entry\n"
		  "  -a, --rebuild-all   build every entry from its files; the originals are not read\n"
		  "      --checksum      compare the files by CRC, not by size and time\n"
		  "  -v                  a line per entry built\n"
		  "  -q                  problems and the summary only\n",
		stderr);
	exit(2);
}

int main(int argc, char** argv)
{
	PackOptions_t opt;
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
			Usage();
		else if(dirCount < 2)
			dirs[dirCount++] = a;
		else
			Usage();
	}
	if(dirCount != 2)
		Usage();
	opt.dumpDir = dirs[0];
	opt.outDir = dirs[1];
	OS_SetArgv0(argv[0]);
	if(!OS_Init()) // the Shift-JIS tables
		return 2;
	return Pack_Run(&opt);
}
