/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bgiscn.c - the scenario decompiler / compiler: the containers of the
 *            1.494+ games, the headerless files of 1.66 / 1.69 and the
 *            16-bit scenes of 1.64 (docs/scenario.md); `make tools`
 *            builds bin/bgiscn
 *
 *   bgiscn --scan SYSPRG.arc                  list the commands of a game
 *   bgiscn -d [-s SYSPRG.arc] [-o OUT] FILE   decompile a scenario file
 *   bgiscn -d [-s SYSPRG.arc] --arc ARCHIVE NAME [-o OUT]
 *   bgiscn -t FILE | --arc ARCHIVE NAME       the token listing
 *   bgiscn -c [-s SYSPRG.arc] [-I DIR]... SOURCE -o OUT
 *                                             compile source to a scenario file
 *   bgiscn -r [-s SYSPRG.arc] (FILE... | --arc ARCHIVE [NAME...])
 *                                             round trip: decompile, compile, compare
 *   bgiscn -l ARCHIVE                         list an archive
 *   bgiscn --pack ARCHIVE -o OUT.arc NAME=FILE...
 *                                             copy an archive with entries replaced or added
 *
 * `-s` gives the game's sysprg.arc (default: sysprg.arc next to the input
 * archive, or ./sysprg.arc); without it the commands are written by number.
 */
#include "bgi/arcread.h"
#include "bgi/os.h"
#include "bgi/scn.h"
#include "bgi/asm.h"

static void Usage(void)
{
	fputs("usage: bgiscn --scan SYSPRG.arc\n"
		  "       bgiscn -d [-s SYSPRG.arc] [-o OUT] [--raw] [--no-verify] (FILE | --arc ARCHIVE NAME)\n"
		  "       bgiscn -t [-s SYSPRG.arc] (FILE | --arc ARCHIVE NAME)\n"
		  "       bgiscn -c [-s SYSPRG.arc] [-I DIR]... SOURCE -o OUT\n"
		  "       bgiscn -r [-s SYSPRG.arc] [-v] (FILE... | --arc ARCHIVE [NAME...])\n"
		  "       bgiscn -l ARCHIVE\n"
		  "       bgiscn --pack ARCHIVE -o OUT.arc NAME=FILE...\n"
		  "  -u, --utf8   the scenario's strings are UTF-8 (a game that runs with UTF-8 text), not Shift-JIS\n",
		stderr);
	exit(2);
}

static uint8_t* ReadFile(const char* path, uint32_t* size)
{
	FILE* f = fopen(path, "rb");
	uint8_t* data;
	long n;
	if(!f)
	{
		fprintf(stderr, "%s: cannot open\n", path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = (uint8_t*)malloc((size_t)n + 1);
	if(fread(data, 1, (size_t)n, f) != (size_t)n)
	{
		fprintf(stderr, "%s: read error\n", path);
		fclose(f);
		free(data);
		return NULL;
	}
	fclose(f);
	data[n] = 0;
	*size = (uint32_t)n;
	return data;
}

static int WriteFile(const char* path, const uint8_t* data, uint32_t size)
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

// the game's command table from -s, or from sysprg.arc beside `near` (an archive path) when that exists
static int LoadCommands(const char* sysprg, const char* near, ScnCmdTable_t* cmds)
{
	char path[0x400];
	memset(cmds, 0, sizeof *cmds);
	if(sysprg)
		return Scn_ScanCommands(sysprg, cmds);
	if(near)
	{
		const char* slash = strrchr(near, '/');
#ifdef _WIN32
		const char* bs = strrchr(near, '\\');
		if(bs && (!slash || bs > slash))
			slash = bs;
#endif
		snprintf(path, sizeof path, "%.*ssysprg.arc", slash ? (int)(slash - near + 1) : 0, near);
	}
	else
		strcpy(path, "sysprg.arc");
	{
		FILE* f = fopen(path, "rb");
		if(!f)
			return 0;
		fclose(f);
	}
	return Scn_ScanCommands(path, cmds);
}

static int LoadEntry(const char* arcPath, const char* name, uint8_t** data, uint32_t* size)
{
	ArcRead_t* a = ArcRead_Open(arcPath);
	const ArcEntry_t* e;
	int dsc;
	if(!a)
		return 0;
	e = ArcRead_Find(a, name);
	if(!e)
	{
		fprintf(stderr, "%s: no entry %s\n", arcPath, name);
		ArcRead_Close(a);
		return 0;
	}
	*data = ArcRead_Load(a, e, size, &dsc);
	ArcRead_Close(a);
	return *data != NULL;
}

// decompile one file image; returns the number of fallbacks, -1 on a load error
static int gVerbose;
static const char* gArc; // the archive the inputs come from (the imports are read from it)

// the functions of the file's imports, read from the archive
static void LoadImports(const ScnFile_t* f, ScnFuncInfo_t** list, int* count)
{
	ArcRead_t* a;
	int i;
	*list = NULL;
	*count = 0;
	if(!gArc)
		return;
	a = ArcRead_Open(gArc);
	if(!a)
		return;
	for(i = -2; i < f->importCount; i++)
	{
		// a raw scene calls the functions of the game's main file (`main`, `mainscript`) by address
		const char* name = i == -2 ? "main" : i == -1 ? "mainscript"
													  : f->imports[i];
		const ArcEntry_t* e = (i < 0 && !f->raw) ? NULL : ArcRead_Find(a, name);
		uint8_t* data;
		uint32_t size;
		int dsc;
		ScnFile_t imp;
		char err[128];
		if(!e)
			continue;
		data = ArcRead_Load(a, e, &size, &dsc);
		if(!data)
			continue;
		if(Scn_Load(data, size, &imp, err, sizeof err))
		{
			Scn_ListFunctions(&imp, list, count);
			Scn_Free(&imp);
		}
		BGI_Free(data);
	}
	ArcRead_Close(a);
}

// decompile one file image; returns the number of fallbacks, -1 on a load error
static int Decompile(const uint8_t* data, uint32_t size, const char* name, const ScnCmdTable_t* cmds, int raw, int noVerify,
	FILE* out)
{
	ScnFile_t f;
	ScnDecOptions_t opt;
	char err[128];
	int n;
	ScnFuncInfo_t* funcs;
	int funcCount;
	if(!Scn_Load(data, size, &f, err, sizeof err))
	{
		fprintf(stderr, "%s: %s\n", name, err);
		return -1;
	}
	LoadImports(&f, &funcs, &funcCount);
	memset(&opt, 0, sizeof opt);
	opt.funcs = funcs;
	opt.funcCount = funcCount;
	opt.cmds = cmds->count ? cmds : NULL;
	opt.name = name;
	opt.rawTokens = raw;
	opt.noVerify = noVerify;
	opt.verbose = gVerbose;
	n = Scn_Decompile(&f, &opt, out);
	Scn_Free(&f);
	free(funcs);
	return n;
}

/* round trip one file image: decompile to memory, compile, compare; 0 when
 * identical, 1 otherwise (with a message); `verbose` writes the source to
 * stderr on a mismatch */
static int RoundTrip(const uint8_t* data, uint32_t size, const char* name, const ScnCmdTable_t* cmds, int verbose)
{
	char* text = NULL;
	size_t textLen = 0;
	FILE* mem;
	ScnCompileResult_t res;
	uint8_t* back;
	uint32_t backSize;
	int fallbacks, ok = 0;
	char tmpPath[0x400];
	snprintf(tmpPath, sizeof tmpPath, "%s", "bgiscn_roundtrip.tmp");
	mem = tmpfile();
	if(!mem)
	{
		fprintf(stderr, "cannot create a temporary file\n");
		return 1;
	}
	fallbacks = Decompile(data, size, name, cmds, 0, 0, mem);
	if(fallbacks < 0)
	{
		fclose(mem);
		return 1;
	}
	textLen = (size_t)ftell(mem);
	fseek(mem, 0, SEEK_SET);
	text = (char*)malloc(textLen + 1);
	if(fread(text, 1, textLen, mem) != textLen)
		textLen = 0;
	text[textLen] = 0;
	fclose(mem);
	Scn_Compile(text, textLen, name, cmds->count ? cmds : NULL, NULL, stderr, &res);
	if(res.errors == 0)
	{
		back = Scn_Save(&res.file, &backSize);
		ok = backSize == size && memcmp(back, data, size) == 0;
		if(!ok)
		{
			uint32_t i;
			for(i = 0; i < size && i < backSize && back[i] == data[i]; i++)
				;
			fprintf(stderr, "%s: round trip differs at byte 0x%x (sizes 0x%x / 0x%x)\n", name, i, size, backSize);
		}
		BGI_Free(back);
		Scn_Free(&res.file);
	}
	else
		fprintf(stderr, "%s: the decompiled source does not compile\n", name);
	if(!ok && verbose)
		fputs(text, stderr);
	else if(fallbacks)
		fprintf(stderr, "%s: %d function(s) written as token blocks\n", name, fallbacks);
	free(text);
	return !ok;
}

int main(int argc, char** argv)
{
	const char* mode = NULL;
	const char* sysprg = NULL;
	const char* outPath = NULL;
	const char* arc = NULL;
	const char* files[4096];
	const char* incDirs[32]; // the -I directories
	int incCount = 0;
	int fileCount = 0, i, raw = 0, noVerify = 0, verbose = 0;
	ScnCmdTable_t cmds;
	for(i = 1; i < argc; i++)
	{
		const char* a = argv[i];
		if(strcmp(a, "--scan") == 0 || strcmp(a, "-d") == 0 || strcmp(a, "-t") == 0 || strcmp(a, "-c") == 0 || strcmp(a, "-r") == 0 || strcmp(a, "-l") == 0 || strcmp(a, "--pack") == 0)
			mode = a;
		else if(strcmp(a, "-s") == 0 && i + 1 < argc)
			sysprg = argv[++i];
		else if(strcmp(a, "-o") == 0 && i + 1 < argc)
			outPath = argv[++i];
		else if(strcmp(a, "--arc") == 0 && i + 1 < argc)
			arc = argv[++i];
		else if(strncmp(a, "-I", 2) == 0 && (a[2] || i + 1 < argc))
		{ // -I DIR or -IDIR: where -c looks for the files of #include lines
			const char* dir = a[2] ? a + 2 : argv[++i];
			if(incCount < (int)(sizeof incDirs / sizeof incDirs[0]))
				incDirs[incCount++] = dir;
		}
		else if(strcmp(a, "-u") == 0 || strcmp(a, "--utf8") == 0)
			Asm_SetDefaultStringEncoding(1); // the scenario's strings are UTF-8
		else if(strcmp(a, "--raw") == 0)
			raw = 1;
		else if(strcmp(a, "--no-verify") == 0)
			noVerify = 1;
		else if(strcmp(a, "-v") == 0)
			verbose = ++gVerbose;
		else if(a[0] == '-' && a[1])
			Usage();
		else if(fileCount < (int)(sizeof files / sizeof files[0]))
			files[fileCount++] = a;
	}
	if(!mode)
		Usage();
	if(!OS_Init()) // the Shift-JIS tables
		return 1;
	if(strcmp(mode, "--scan") == 0)
	{
		if(fileCount != 1)
			Usage();
		if(!Scn_ScanCommands(files[0], &cmds))
			return 1;
		Scn_WriteCommands(&cmds, stdout);
		Scn_FreeCommands(&cmds);
		return 0;
	}
	if(strcmp(mode, "-l") == 0)
	{
		ArcRead_t* a;
		uint32_t k;
		if(fileCount != 1)
			Usage();
		a = ArcRead_Open(files[0]);
		if(!a)
			return 1;
		for(k = 0; k < a->count; k++)
			printf("  %-32s %10u bytes\n", a->entries[k].name, a->entries[k].size);
		ArcRead_Close(a);
		return 0;
	}
	if(strcmp(mode, "--pack") == 0)
	{
		ArcRead_t* a;
		ArcWriteEntry_t* entries;
		uint8_t** loaded;
		uint32_t k, outSize;
		int count, j, ok;
		uint8_t* out;
		if(fileCount < 1 || !outPath)
			Usage();
		a = ArcRead_Open(files[0]);
		if(!a)
			return 1;
		entries = (ArcWriteEntry_t*)calloc(a->count + (uint32_t)fileCount, sizeof *entries);
		loaded = (uint8_t**)calloc(a->count + (uint32_t)fileCount, sizeof *loaded);
		count = (int)a->count;
		// the existing entries as stored (compressed or not), then the replacements
		for(k = 0; k < a->count; k++)
		{
			const ArcEntry_t* e = &a->entries[k];
			entries[k].name = e->name;
			loaded[k] = (uint8_t*)malloc(e->size + 1);
			if(fseek(a->f, (long)e->offset, SEEK_SET) != 0 || fread(loaded[k], 1, e->size, a->f) != e->size)
			{
				fprintf(stderr, "%s: cannot read %s\n", files[0], e->name);
				return 1;
			}
			entries[k].data = loaded[k];
			entries[k].size = e->size;
		}
		for(j = 1; j < fileCount; j++)
		{
			const char* eq = strchr(files[j], '=');
			char name[0x61];
			uint32_t size;
			uint8_t* data;
			int found = 0;
			if(!eq || eq == files[j] || (size_t)(eq - files[j]) >= sizeof name)
			{
				fprintf(stderr, "%s: NAME=FILE expected\n", files[j]);
				return 1;
			}
			snprintf(name, sizeof name, "%.*s", (int)(eq - files[j]), files[j]);
			data = ReadFile(eq + 1, &size);
			if(!data)
				return 1;
			for(k = 0; k < a->count; k++)
				if(strcmp(entries[k].name, name) == 0)
				{
					entries[k].data = data;
					entries[k].size = size;
					found = 1;
				}
			if(!found)
			{
				entries[count].name = files[j]; // the NAME part: cut at '=' below
				((char*)files[j])[eq - files[j]] = 0;
				entries[count].data = data;
				entries[count].size = size;
				count++;
			}
		}
		out = ArcWrite_Build(entries, count, a->arc20, &outSize);
		ok = WriteFile(outPath, out, outSize);
		printf("%s: %d entries, %u bytes%s\n", outPath, count, outSize, a->arc20 ? " (BURIKO ARC20)" : " (PackFile)");
		free(out);
		for(k = 0; k < a->count; k++)
			free(loaded[k]);
		free(loaded);
		free(entries);
		ArcRead_Close(a);
		return ok ? 0 : 1;
	}
	if(strcmp(mode, "-c") == 0)
	{
		uint8_t* text;
		uint32_t len;
		ScnCompileResult_t res;
		PrepIncludes_t inc;
		uint8_t* out;
		uint32_t outSize;
		int ok;
		if(fileCount != 1 || !outPath)
			Usage();
		text = ReadFile(files[0], &len);
		if(!text)
			return 1;
		LoadCommands(sysprg, NULL, &cmds);
		memset(&inc, 0, sizeof inc);
		inc.dirs = incDirs;
		inc.dirCount = incCount;
		Scn_Compile((const char*)text, len, files[0], cmds.count ? &cmds : NULL, &inc, stderr, &res);
		free(text);
		if(res.errors)
		{
			fprintf(stderr, "%d error(s)\n", res.errors);
			Scn_FreeCommands(&cmds);
			return 1;
		}
		out = Scn_Save(&res.file, &outSize);
		ok = WriteFile(outPath, out, outSize);
		BGI_Free(out);
		Scn_Free(&res.file);
		Scn_FreeCommands(&cmds);
		return ok ? 0 : 1;
	}
	// -d, -t, -r: inputs are files, or entries of --arc
	LoadCommands(sysprg, arc, &cmds);
	gArc = arc;
	if(strcmp(mode, "-r") == 0)
	{
		int failed = 0, total = 0;
		if(arc)
		{
			ArcRead_t* a = ArcRead_Open(arc);
			uint32_t k;
			if(!a)
				return 1;
			for(k = 0; k < a->count; k++)
			{
				const ArcEntry_t* e = &a->entries[k];
				uint8_t* data;
				uint32_t size;
				int dsc, want = fileCount == 0, j;
				for(j = 0; j < fileCount; j++)
					if(strcmp(files[j], e->name) == 0)
						want = 1;
				if(!want)
					continue;
				data = ArcRead_Load(a, e, &size, &dsc);
				if(!data)
					continue;
				{ // a scenario file: the Buriko container, or the headerless one (the data tables are neither)
					ScnFile_t probe;
					char err[128];
					if(Scn_Load(data, size, &probe, err, sizeof err))
					{
						Scn_Free(&probe);
						total++;
						failed += RoundTrip(data, size, e->name, &cmds, verbose);
					}
				}
				BGI_Free(data);
			}
			ArcRead_Close(a);
		}
		else
			for(i = 0; i < fileCount; i++)
			{
				uint32_t size;
				uint8_t* data = ReadFile(files[i], &size);
				if(!data)
				{
					failed++;
					continue;
				}
				total++;
				failed += RoundTrip(data, size, files[i], &cmds, verbose);
				free(data);
			}
		printf("%d files, %d failed\n", total, failed);
		Scn_FreeCommands(&cmds);
		return failed ? 1 : 0;
	}
	{
		uint8_t* data;
		uint32_t size;
		FILE* out = stdout;
		const char* name;
		int n;
		if(arc)
		{
			if(fileCount != 1)
				Usage();
			name = files[0];
			if(!LoadEntry(arc, name, &data, &size))
				return 1;
		}
		else
		{
			if(fileCount != 1)
				Usage();
			name = strrchr(files[0], '/') ? strrchr(files[0], '/') + 1 : files[0];
			data = ReadFile(files[0], &size);
			if(!data)
				return 1;
		}
		if(outPath)
		{
			ScnFile_t probe;
			char err[128];
			if(!Scn_Load(data, size, &probe, err, sizeof err))
			{ // not a scenario (the data tables of some archives): no empty output file
				fprintf(stderr, "%s: %s\n", name, err);
				return 1;
			}
			Scn_Free(&probe);
			out = fopen(outPath, "wb");
			if(!out)
			{
				fprintf(stderr, "%s: cannot write\n", outPath);
				return 1;
			}
		}
		n = Decompile(data, size, name, &cmds, strcmp(mode, "-t") == 0 || raw, noVerify, out);
		if(out != stdout)
			fclose(out);
		if(arc)
			BGI_Free(data);
		else
			free(data);
		Scn_FreeCommands(&cmds);
		return n < 0 ? 1 : 0;
	}
}
