/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bpasm.c - the assembler / disassembler and the decompiler / compiler of
 *           Buriko programs, as a command
 *
 *   bpasm -d [-e ENGINE] [-b] [-O] FILE._bp            disassemble a file
 *   bpasm -d [-e ENGINE] [-b] [-O] --arc ARCHIVE NAME  ... an archive entry
 *   bpasm -a [-e ENGINE] FILE.s -o OUT._bp             assemble
 *   bpasm -r [-e ENGINE] FILE._bp...                   round trip: disassemble,
 *   bpasm -r [-e ENGINE] --arc ARCHIVE [NAME...]         assemble, compare
 *   bpasm -D [-e ENGINE] [--no-verify] FILE._bp        decompile to source (docs/bpc.md)
 *   bpasm -D [-e ENGINE] --arc ARCHIVE NAME            ... an archive entry
 *   bpasm -C [-e ENGINE] [-I DIR]... SOURCE -o OUT._bp compile source
 *   bpasm -R [-e ENGINE] (FILE._bp... | --arc ARCHIVE [NAME...])
 *                                                      round trip through the decompiler
 *   bpasm -l ARCHIVE                                   list the entries
 *
 * ENGINE is a profile name (`bgi --list-engines`: "1.69/444", "1.553",
 * ..) or "any" (the default: every opcode of every build, the newest
 * meaning of a reused number).  -b lists the instruction bytes, -O the
 * offsets, -v also reports the programs that round-trip cleanly (otherwise
 * only failures and listings with problems are mentioned) and the size of
 * an assembled file.  The listing goes to standard output unless -o names
 * a file.  -I adds a directory in which -C looks for the files of
 * `#include` lines (after the directory of the including file; the option
 * may be repeated).  docs/asm.md describes the listing.
 *
 * A stand-alone program on asm.h (the assembler and disassembler),
 * arcread.h (the archive reader of the tools) and the OS layer (for the
 * Shift-JIS conversions).  The exit status is 0 on success, 1 when a file
 * could not be handled or a round trip failed, 2 for a usage error.
 */
#include "bgi/asm.h"
#include "bgi/bpc.h"
#include "bgi/arcread.h"
#include "bgi/os.h"

// the usage message; exits with status 2
static void Usage(void)
{
	fprintf(stderr,
		"usage: bpasm -d [-e ENGINE] [-b] [-O] [-o OUT] (FILE._bp | --arc ARCHIVE NAME)\n"
		"       bpasm -a [-e ENGINE] FILE.s -o OUT._bp\n"
		"       bpasm -r [-e ENGINE] (FILE._bp... | --arc ARCHIVE [NAME...])\n"
		"       bpasm -D [-e ENGINE] [--no-verify] [-v] [-o OUT] (FILE._bp | --arc ARCHIVE NAME)\n"
		"       bpasm -C [-e ENGINE] [-I DIR]... SOURCE -o OUT._bp\n"
		"       bpasm -R [-e ENGINE] [-v] (FILE._bp... | --arc ARCHIVE [NAME...])\n"
		"       bpasm -l ARCHIVE\n"
		"  -u, --utf8   the program's strings are UTF-8 (a game that runs with UTF-8 text), not Shift-JIS\n");
	exit(2);
}

/* the whole file into a new buffer (BGI_Free it) with a NUL after the data,
 * `*size` its length; NULL with a message on stderr when it cannot be read */
static uint8_t* ReadFile(const char* path, uint32_t* size)
{
	FILE* f = fopen(path, "rb");
	uint8_t* buf;
	long n;
	if(!f)
	{
		fprintf(stderr, "%s: cannot open\n", path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = (uint8_t*)BGI_Alloc((size_t)n + 1);
	if(fread(buf, 1, (size_t)n, f) != (size_t)n)
	{
		fprintf(stderr, "%s: read error\n", path);
		fclose(f);
		BGI_Free(buf);
		return NULL;
	}
	fclose(f);
	buf[n] = 0;
	*size = (uint32_t)n;
	return buf;
}

// write `size` bytes to a new file: 1 on success, 0 with a message on stderr
static int WriteFile(const char* path, const void* data, uint32_t size)
{
	FILE* f = fopen(path, "wb");
	if(!f || fwrite(data, 1, size, f) != size)
	{
		fprintf(stderr, "%s: cannot write\n", path);
		if(f)
			fclose(f);
		return 0;
	}
	fclose(f);
	return 1;
}

/* the generation of the -e argument: "any" (GEN_COUNT), a profile name
 * ("1.69/444", which also selects that profile as the engine's) or a
 * generation name of kGenNames; exits with status 2 when it is none */
static EngineGen_t ParseEngine(const char* name)
{
	int g;
	if(strcmp(name, "any") == 0)
		return GEN_COUNT;
	if(Engine_SelectProfile(name))
		return gEngine->gen;
	for(g = 0; g < GEN_COUNT; g++)
		if(strcmp(kGenNames[g], name) == 0)
			return (EngineGen_t)g;
	fprintf(stderr, "bpasm: unknown engine %s (see bgi --list-engines)\n", name);
	exit(2);
}

/* the program disassembled into memory, then assembled again: 0 when the
 * bytes come back identical, 1 when the listing does not assemble or
 * differs (reported on stdout with the first differing offset).  Problems
 * the disassembler noted are mentioned even when the bytes match. */
static int RoundTrip(const uint8_t* file, uint32_t size, const char* name, EngineGen_t gen, int verbose)
{
	FILE* tmp = tmpfile();
	DisOptions_t opt;
	AsmResult_t res;
	char* text;
	long n;
	int problems, rc = 0;
	if(!tmp)
	{
		fprintf(stderr, "cannot make a temporary file\n");
		return 1;
	}
	memset(&opt, 0, sizeof opt);
	opt.gen = gen;
	opt.name = name;
	problems = Dis_Program(file, size, &opt, tmp);
	n = ftell(tmp);
	text = (char*)BGI_Alloc((size_t)n + 1);
	fseek(tmp, 0, SEEK_SET);
	if(fread(text, 1, (size_t)n, tmp) != (size_t)n)
		n = 0;
	fclose(tmp);
	text[n] = 0;
	Asm_Assemble(text, (size_t)n, gen, stderr, &res);
	if(!res.file)
	{
		printf("%-24s FAILED: the listing does not assemble (%d errors)\n", name, res.errors);
		rc = 1;
	}
	else if(res.size != size || memcmp(res.file, file, size) != 0)
	{
		uint32_t i, lim = res.size < size ? res.size : size;
		for(i = 0; i < lim && res.file[i] == file[i]; i++)
			;
		printf("%-24s MISMATCH: %u bytes back from %u, first difference at file offset 0x%x\n", name, (unsigned)res.size,
			(unsigned)size, (unsigned)i);
		rc = 1;
	}
	else if(problems)
		printf("%-24s ok (%u bytes, %d problem%s noted in the listing)\n", name, (unsigned)size, problems,
			problems == 1 ? "" : "s");
	else if(verbose)
		printf("%-24s ok (%u bytes)\n", name, (unsigned)size);
	BGI_Free(res.file);
	BGI_Free(text);
	return rc;
}

/* the program decompiled into memory, compiled again and compared: 0
 * when identical (the number of functions written as blocks is
 * mentioned), 1 otherwise */
static int RoundTripSource(const uint8_t* file, uint32_t size, const char* name, EngineGen_t gen, int verbose)
{
	FILE* tmp = tmpfile();
	BpcDecOptions_t opt;
	BpcResult_t res;
	char* text;
	long n;
	int blocks, rc = 0;
	if(!tmp)
	{
		fprintf(stderr, "cannot make a temporary file\n");
		return 1;
	}
	memset(&opt, 0, sizeof opt);
	opt.gen = gen;
	opt.name = name;
	opt.verbose = verbose;
	blocks = Bpc_Decompile(file, size, &opt, tmp);
	n = ftell(tmp);
	text = (char*)BGI_Alloc((size_t)n + 1);
	fseek(tmp, 0, SEEK_SET);
	if(fread(text, 1, (size_t)n, tmp) != (size_t)n)
		n = 0;
	fclose(tmp);
	text[n] = 0;
	if(blocks < 0)
	{
		printf("%-24s FAILED: no usable header\n", name);
		BGI_Free(text);
		return 1;
	}
	Bpc_Compile(text, (size_t)n, name, gen, NULL, stderr, &res);
	if(!res.file)
	{
		printf("%-24s FAILED: the source does not compile (%d errors)\n", name, res.errors);
		rc = 1;
	}
	else if(res.size != size || memcmp(res.file, file, size) != 0)
	{
		uint32_t i, lim = res.size < size ? res.size : size;
		for(i = 0; i < lim && res.file[i] == file[i]; i++)
			;
		printf("%-24s MISMATCH: %u bytes back from %u, first difference at file offset 0x%x\n", name, (unsigned)res.size,
			(unsigned)size, (unsigned)i);
		rc = 1;
	}
	else if(blocks)
		printf("%-24s ok (%u bytes, %d function%s as __asm blocks)\n", name, (unsigned)size, blocks, blocks == 1 ? "" : "s");
	else if(verbose)
		printf("%-24s ok (%u bytes)\n", name, (unsigned)size);
	BGI_Free(res.file);
	BGI_Free(text);
	return rc;
}

/* the command: the options, then one of the modes - 'l' lists an archive,
 * 'd' disassembles one file or entry, 'a' assembles one file, 'r' round
 * trips every named file or every program of an archive (all of its
 * entries that have a program header, or only the named ones) */
int main(int argc, char** argv)
{
	int mode = 0, showBytes = 0, showOffsets = 0, verbose = 0, noVerify = 0, i, rc = 0;
	const char* engine = "any";
	const char* arc = NULL;
	const char* outPath = NULL;
	const char* files[256]; // the positional arguments (more are ignored)
	int fileCount = 0;
	const char* incDirs[32]; // the -I directories
	int incCount = 0;
	EngineGen_t gen;

	for(i = 1; i < argc; i++)
	{
		const char* a = argv[i];
		if(strcmp(a, "-d") == 0 || strcmp(a, "-a") == 0 || strcmp(a, "-r") == 0 || strcmp(a, "-l") == 0 || strcmp(a, "-D") == 0 || strcmp(a, "-C") == 0 || strcmp(a, "-R") == 0)
			mode = a[1]; // the mode is its option letter
		else if(strcmp(a, "--no-verify") == 0)
			noVerify = 1;
		else if(strcmp(a, "-e") == 0 && i + 1 < argc)
			engine = argv[++i];
		else if(strcmp(a, "-o") == 0 && i + 1 < argc)
			outPath = argv[++i];
		else if(strcmp(a, "--arc") == 0 && i + 1 < argc)
			arc = argv[++i];
		else if(strncmp(a, "-I", 2) == 0 && (a[2] || i + 1 < argc))
		{ // -I DIR or -IDIR
			const char* dir = a[2] ? a + 2 : argv[++i];
			if(incCount < (int)BGI_COUNTOF(incDirs))
				incDirs[incCount++] = dir;
		}
		else if(strcmp(a, "-u") == 0 || strcmp(a, "--utf8") == 0)
			Asm_SetDefaultStringEncoding(1); // the program's strings are UTF-8
		else if(strcmp(a, "-b") == 0)
			showBytes = 1;
		else if(strcmp(a, "-O") == 0)
			showOffsets = 1;
		else if(strcmp(a, "-v") == 0)
			verbose++;
		else if(a[0] == '-' && a[1])
			Usage();
		else if(fileCount < 256)
			files[fileCount++] = a;
	}
	if(!mode)
		Usage();
	if(!OS_Init()) // the Shift-JIS tables
		return 1;
	gen = ParseEngine(engine);

	if(mode == 'l')
	{ // the entries of an archive with their sizes and offsets, names in UTF-8
		ArcRead_t* ar;
		uint32_t k;
		if(fileCount != 1)
			Usage();
		ar = ArcRead_Open(files[0]);
		if(!ar)
			return 1;
		printf("%s: %u entries (%s)\n", files[0], (unsigned)ar->count, ar->arc20 ? "BURIKO ARC20" : "PackFile");
		for(k = 0; k < ar->count; k++)
		{
			char utf[0x200];
			OS_SjisToUtf8(ar->entries[k].name, utf, sizeof utf);
			printf("  %-32s %10u bytes at 0x%08x\n", utf, (unsigned)ar->entries[k].size, (unsigned)ar->entries[k].offset);
		}
		ArcRead_Close(ar);
	}
	else if(mode == 'd')
	{ // one program, from a file or an archive entry (decoded from DSC when stored so), to stdout or -o
		uint8_t* file;
		uint32_t size;
		int wasDsc = 0;
		DisOptions_t opt;
		FILE* out = stdout;
		const char* name;
		if(fileCount != 1)
			Usage();
		name = files[0];
		if(arc)
		{
			ArcRead_t* ar = ArcRead_Open(arc);
			const ArcEntry_t* e;
			if(!ar)
				return 1;
			e = ArcRead_Find(ar, name);
			if(!e)
			{
				fprintf(stderr, "%s: no entry %s\n", arc, name);
				return 1;
			}
			file = ArcRead_Load(ar, e, &size, &wasDsc);
			ArcRead_Close(ar);
			if(!file)
				return 1;
		}
		else
			file = ReadFile(name, &size);
		if(!file)
			return 1;
		if(outPath)
		{
			out = fopen(outPath, "w");
			if(!out)
			{
				fprintf(stderr, "%s: cannot write\n", outPath);
				return 1;
			}
		}
		memset(&opt, 0, sizeof opt);
		opt.gen = gen;
		opt.showBytes = showBytes;
		opt.showOffsets = showOffsets;
		opt.name = name;
		rc = Dis_Program(file, size, &opt, out) ? 1 : 0; // a listing with problems is still written
		if(out != stdout)
			fclose(out);
		BGI_Free(file);
	}
	else if(mode == 'D')
	{ // one program decompiled to source, to stdout or -o
		uint8_t* file;
		uint32_t size;
		int wasDsc = 0;
		BpcDecOptions_t opt;
		FILE* out = stdout;
		const char* name;
		if(fileCount != 1)
			Usage();
		name = files[0];
		if(arc)
		{
			ArcRead_t* ar = ArcRead_Open(arc);
			const ArcEntry_t* e;
			if(!ar)
				return 1;
			e = ArcRead_Find(ar, name);
			if(!e)
			{
				fprintf(stderr, "%s: no entry %s\n", arc, name);
				return 1;
			}
			file = ArcRead_Load(ar, e, &size, &wasDsc);
			ArcRead_Close(ar);
			if(!file)
				return 1;
		}
		else
			file = ReadFile(name, &size);
		if(!file)
			return 1;
		if(outPath)
		{
			out = fopen(outPath, "w");
			if(!out)
			{
				fprintf(stderr, "%s: cannot write\n", outPath);
				return 1;
			}
		}
		memset(&opt, 0, sizeof opt);
		opt.gen = gen;
		opt.name = strrchr(name, '/') ? strrchr(name, '/') + 1 : name;
		opt.noVerify = noVerify;
		opt.verbose = verbose;
		rc = Bpc_Decompile(file, size, &opt, out) < 0 ? 1 : 0;
		if(out != stdout)
			fclose(out);
		BGI_Free(file);
	}
	else if(mode == 'C')
	{ // one source to the program file -o names
		uint8_t* text;
		uint32_t size;
		BpcResult_t res;
		PrepIncludes_t inc;
		if(fileCount != 1 || !outPath)
			Usage();
		text = ReadFile(files[0], &size);
		if(!text)
			return 1;
		memset(&inc, 0, sizeof inc);
		inc.dirs = incDirs;
		inc.dirCount = incCount;
		Bpc_Compile((const char*)text, size, files[0], gen, &inc, stderr, &res);
		BGI_Free(text);
		BGI_Free(res.labels);
		if(!res.file)
		{
			fprintf(stderr, "%s: %d error%s\n", files[0], res.errors, res.errors == 1 ? "" : "s");
			return 1;
		}
		if(!WriteFile(outPath, res.file, res.size))
			rc = 1;
		else if(verbose)
			printf("%s: %u bytes%s\n", outPath, (unsigned)res.size, res.warnings ? " (with warnings)" : "");
		BGI_Free(res.file);
	}
	else if(mode == 'a')
	{ // one listing to the program file -o names; messages go to stderr
		uint8_t* text;
		uint32_t size;
		AsmResult_t res;
		if(fileCount != 1 || !outPath)
			Usage();
		text = ReadFile(files[0], &size);
		if(!text)
			return 1;
		Asm_Assemble((const char*)text, size, gen, stderr, &res);
		BGI_Free(text);
		if(!res.file)
		{
			fprintf(stderr, "%s: %d error%s\n", files[0], res.errors, res.errors == 1 ? "" : "s");
			return 1;
		}
		if(!WriteFile(outPath, res.file, res.size))
			rc = 1;
		else if(verbose)
			printf("%s: %u bytes%s\n", outPath, (unsigned)res.size, res.warnings ? " (with warnings)" : "");
		BGI_Free(res.file);
	}
	else if(mode == 'r' || mode == 'R')
	{ // round trip: every named file, or the programs of an archive (all of them, or the named entries)
		int total = 0, failed = 0;
		if(arc)
		{
			ArcRead_t* ar = ArcRead_Open(arc);
			uint32_t k;
			if(!ar)
				return 1;
			for(k = 0; k < ar->count; k++)
			{
				const ArcEntry_t* e = &ar->entries[k];
				uint8_t* file;
				uint32_t size;
				int wasDsc, want = fileCount == 0;
				for(i = 0; i < fileCount && !want; i++)
					want = ArcRead_Find(ar, files[i]) == e;
				if(!want)
					continue;
				file = ArcRead_Load(ar, e, &size, &wasDsc);
				if(!file)
					continue; // an entry that cannot be read is skipped silently and not counted
				// only the programs: the 16-byte header (code offset 16, the size, two zero words)
				if(size >= 16 && file[0] == 0x10 && file[1] == 0 && file[2] == 0 && file[3] == 0 &&
					memcmp(file + 8, "\0\0\0\0\0\0\0\0", 8) == 0)
				{
					total++;
					failed += mode == 'R' ? RoundTripSource(file, size, e->name, gen, verbose) : RoundTrip(file, size, e->name, gen, verbose);
				}
				BGI_Free(file);
			}
			ArcRead_Close(ar);
		}
		else
		{ // files on disk: one that cannot be read counts as failed
			for(i = 0; i < fileCount; i++)
			{
				uint8_t* file;
				uint32_t size;
				file = ReadFile(files[i], &size);
				if(!file)
				{
					failed++;
					total++;
					continue;
				}
				total++;
				failed += mode == 'R' ? RoundTripSource(file, size, files[i], gen, verbose) : RoundTrip(file, size, files[i], gen, verbose);
				BGI_Free(file);
			}
		}
		printf("%d program%s, %d failed\n", total, total == 1 ? "" : "s", failed);
		rc = failed ? 1 : 0;
	}
	OS_Shutdown();
	return rc;
}
