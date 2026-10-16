/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dump.c - Dump_Run: every archive of a game directory into a directory
 *          of editable files, with the description the pack tool builds
 *          the archives from again (dump.h, docs/dump.md)
 *
 * The run: the directory is listed and its "*.arc" files sorted; the boot
 * program (a loose ipl._bp, else the one in system.arc) is matched against
 * games.json for the engine profile; the scenario commands are read from
 * sysprg.arc; then each archive is dumped in turn and its part of the
 * description collected, and "bgidump.json" is written last - a dump that
 * was interrupted has none and is not mistaken for a whole one.
 *
 * An archive's entries are shared out among worker processes (OS_RunWorkers:
 * entry i goes to worker i modulo their number), each with its own handle
 * on the archive.  A worker writes the files of its entries and one line
 * per entry - the entry's JSON object - into a part file, which the parent
 * reads back in entry order.  The file names are settled before the
 * workers start, from the entry names alone, so that no two entries can
 * end up with the same file whatever they turn out to be.
 */
#include "dump_internal.h"
#include "bgi/codec.h"

// ---- what the converters call ------------------------------------------------------------------------

void Dump_Note(DumpCtx_t* c, const char* fmt, ...)
{
	char msg[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	fprintf(c->log, "%s/%s: %s\n", c->arcName, c->entryName, msg);
}

int Dump_EmitFile(DumpCtx_t* c, const char* key, const char* rel, const void* data, size_t size)
{
	char path[DUMP_PATH_MAX];
	OsNativeStat_t st;
	DumpFs_Join(path, sizeof path, c->arcDir, rel);
	if(!DumpFs_Write(path, data, size) || OS_NativeStat(path, &st) != OS_NATIVE_FILE)
	{
		Dump_Note(c, "cannot write %s", rel);
		c->failed = 1;
		return 0;
	}
	if(key)
		DumpText_Printf(c->meta, ", \"%s\": ", key);
	DumpText_Add(c->meta, "{\"path\": ");
	DumpText_AddString(c->meta, rel);
	DumpText_Printf(c->meta, ", \"size\": %llu, \"mtime\": %lld, \"crc\": \"%08x\"}", (unsigned long long)size, (long long)st.mtime,
		(unsigned)BGI_Crc32(0, data, size));
	return 1;
}

int Dump_MakeDir(DumpCtx_t* c, const char* rel)
{
	char path[DUMP_PATH_MAX];
	DumpFs_Join(path, sizeof path, c->arcDir, rel);
	if(!OS_NativeMkdir(path))
	{
		Dump_Note(c, "cannot create the directory %s", rel);
		c->failed = 1;
		return 0;
	}
	return 1;
}

// ---- lists of names ---------------------------------------------------------------------------------

typedef struct NameList
{
	char** names;
	int count, cap;
} NameList_t;

static void NameList_Add(NameList_t* l, const char* name)
{
	if(l->count == l->cap)
	{
		int cap = l->cap ? l->cap * 2 : 64;
		char** names = (char**)BGI_Calloc(sizeof(char*) * (size_t)cap);
		if(l->count)
			memcpy(names, l->names, sizeof(char*) * (size_t)l->count);
		BGI_Free(l->names);
		l->names = names;
		l->cap = cap;
	}
	l->names[l->count++] = BGI_Strdup(name);
}

static void NameList_Free(NameList_t* l)
{
	int i;
	for(i = 0; i < l->count; i++)
		BGI_Free(l->names[i]);
	BGI_Free(l->names);
	memset(l, 0, sizeof *l);
}

// OS_NativeList's callback: the files of the directory
static void CollectFile(void* ctx, const char* name, int isDir)
{
	if(!isDir)
		NameList_Add((NameList_t*)ctx, name);
}

// order without regard to ASCII case, the exact spelling deciding between equals
static int CompareNames(const void* a, const void* b)
{
	const uint8_t* x = *(const uint8_t* const*)a;
	const uint8_t* y = *(const uint8_t* const*)b;
	size_t i;
	for(i = 0; x[i] && y[i]; i++)
	{
		int p = x[i] >= 'A' && x[i] <= 'Z' ? x[i] + 32 : x[i], q = y[i] >= 'A' && y[i] <= 'Z' ? y[i] + 32 : y[i];
		if(p != q)
			return p - q;
	}
	if(x[i] != y[i])
		return (int)x[i] - (int)y[i];
	return strcmp((const char*)x, (const char*)y);
}

// the name of the list that equals `name` without regard to case, or NULL
static const char* FindName(const NameList_t* l, const char* name)
{
	int i;
	for(i = 0; i < l->count; i++)
		if(Dump_EqualNoCase(l->names[i], name))
			return l->names[i];
	return NULL;
}

// ---- file names for the entries ---------------------------------------------------------------------

/* What two entries must not share: the name without one ending the
 * converters add or take away, in lower case.  "title" and "title.png"
 * could both want title.png; "title._bp" becomes title.bpc. */
static void StemKey(const char* safe, char* key, size_t n)
{
	static const char* const endings[] = {"._bp", ".png", ".json", ".ogg", ".wav", ".bsc", ".bpc", ".bin"};
	size_t len, i;
	snprintf(key, n, "%s", safe);
	for(i = 0; key[i]; i++)
		if(key[i] >= 'A' && key[i] <= 'Z')
			key[i] = (char)(key[i] + 32);
	len = strlen(key);
	for(i = 0; i < BGI_COUNTOF(endings); i++)
	{
		size_t m = strlen(endings[i]);
		if(len > m && strcmp(key + len - m, endings[i]) == 0)
		{
			key[len - m] = 0;
			break;
		}
	}
}

// a set of strings: open addressing over a table twice the size of what goes in
typedef struct KeySet
{
	char** slots;
	uint32_t mask;
} KeySet_t;

static int KeySet_Add(KeySet_t* s, const char* key) // 1 when the key was new
{
	uint32_t h = BGI_Crc32(0, key, strlen(key)) & s->mask;
	while(s->slots[h])
	{
		if(strcmp(s->slots[h], key) == 0)
			return 0;
		h = (h + 1) & s->mask;
	}
	s->slots[h] = BGI_Strdup(key);
	return 1;
}

/* the file-name stem of every entry of the archive (BGI_Free each and the
 * list): the entry's name made safe for a file system, with "~2", "~3" ..
 * appended where an earlier entry has the same StemKey */
static char** MakeBases(const ArcRead_t* a)
{
	char** bases = (char**)BGI_Calloc(sizeof(char*) * ((size_t)a->count + 1));
	KeySet_t set;
	uint32_t i, cap = 16;
	while(cap < a->count * 2 + 2)
		cap *= 2;
	set.slots = (char**)BGI_Calloc(sizeof(char*) * cap);
	set.mask = cap - 1;
	for(i = 0; i < a->count; i++)
	{
		char utf[DUMP_NAME_MAX], safe[DUMP_NAME_MAX], key[DUMP_NAME_MAX], candidate[DUMP_NAME_MAX + 16];
		int n = 1;
		OS_SjisToUtf8(a->entries[i].name, utf, sizeof utf);
		Dump_SafeName(utf, safe, sizeof safe);
		snprintf(candidate, sizeof candidate, "%s", safe);
		for(;;)
		{
			StemKey(candidate, key, sizeof key);
			if(KeySet_Add(&set, key))
				break;
			snprintf(candidate, sizeof candidate, "%s~%d", safe, ++n);
		}
		bases[i] = BGI_Strdup(candidate);
	}
	for(i = 0; i < cap; i++)
		BGI_Free(set.slots[i]);
	BGI_Free(set.slots);
	return bases;
}

// ---- one entry ----------------------------------------------------------------------------------------

/* Dump entry `index` of the archive: read it, take the DSC layer off, let
 * the converters have it in turn, and write its JSON object (one line) into
 * `line`.  Returns the kind it was dumped as. */
static DumpKind_t DumpEntry(DumpCtx_t* c, const ArcEntry_t* e, const char* base, DumpText_t* line)
{
	uint8_t* stored = (uint8_t*)BGI_Alloc((size_t)e->size + 1);
	uint8_t* decoded = NULL;
	const uint8_t* data = stored;
	uint32_t size = e->size;
	char utf[DUMP_NAME_MAX], back[DUMP_NAME_MAX];
	DumpText_t members;
	DumpKind_t kind = DUMP_DATA;
	int dsc = 0, readOk;

	OS_SjisToUtf8(e->name, utf, sizeof utf);
	c->entryName = utf;
	c->base = base;
	memset(&members, 0, sizeof members);
	c->meta = &members;
	readOk = OS_NativeSeek(c->arc->f, e->offset) && fread(stored, 1, e->size, c->arc->f) == e->size;
	if(!readOk)
	{
		Dump_Note(c, "cannot read the entry from the archive");
		c->failed = 1;
		memset(stored, 0, e->size);
	}
	if(readOk && e->size >= 0x220 && BGI_IsDsc(stored))
	{
		uint32_t outSize = BGI_DscOutSize(stored);
		decoded = (uint8_t*)BGI_Calloc((size_t)outSize + 16);
		if(BGI_DscDecode(stored, decoded) == outSize)
		{
			data = decoded;
			size = outSize;
			dsc = 1;
		}
		else
		{
			Dump_Note(c, "the DSC stream does not decode to its announced size: kept as it is");
			c->problems++;
		}
	}

	// by what the data says first, then by the name, then by what loads
	if(size >= 0x30 && BGI_IsCbg(data) && Dump_Image(c, data, size))
		kind = DUMP_IMAGE;
	else if(Dump_Sound(c, data, size))
		kind = DUMP_SOUND;
	else if(Dump_Movie(c, data, size))
		kind = DUMP_MOVIE;
	else if(Dump_Program(c, data, size))
		kind = DUMP_PROGRAM;
	else if(Dump_Table(c, data, size))
		kind = DUMP_TABLE;
	else if(Dump_Sdc(c, data, size))
		kind = DUMP_SDC;
	else if(!(size >= 0x30 && BGI_IsCbg(data)) && Dump_Image(c, data, size))
		kind = DUMP_IMAGE;
	else if(Dump_Scenario(c, data, size))
		kind = DUMP_SCENARIO;
	else
		Dump_Data(c, data, size);

	DumpText_Add(line, "{\"name\": ");
	DumpText_AddString(line, utf);
	// a name that does not survive the trip through UTF-8 is kept as its bytes as well
	OS_Utf8ToSjis(utf, back, sizeof back);
	if(strcmp(back, e->name) != 0)
	{
		char hex[0x61 * 2 + 1];
		Dump_Hex(hex, (const uint8_t*)e->name, strlen(e->name));
		DumpText_Printf(line, ", \"name_sjis\": \"%s\"", hex);
	}
	DumpText_Printf(line, ", \"kind\": \"%s\", \"offset\": %u, \"size\": %u, \"crc\": \"%08x\", \"dsc\": %s", kDumpKindNames[kind], (unsigned)e->offset,
		(unsigned)e->size, (unsigned)BGI_Crc32(0, stored, e->size), dsc ? "true" : "false");
	if(members.len)
		DumpText_AddN(line, members.p, members.len);
	DumpText_Add(line, "}");
	if(c->opt->verbose >= 2)
		fprintf(c->log, "  %s: %s\n", utf, kDumpKindNames[kind]);
	DumpText_Free(&members);
	BGI_Free(decoded);
	BGI_Free(stored);
	c->entryName = "";
	c->meta = NULL;
	return kind;
}

// ---- one archive, by its workers --------------------------------------------------------------------

typedef struct ArcWork
{
	DumpCtx_t ctx;          // what every worker starts from
	const char* arcPath;    // the archive (native)
	const ArcRead_t* index; // the parent's index: the entries
	char** bases;           // the entries' file-name stems
	int workers;
} ArcWork_t;

// the path of a worker's temporary file in the dump's directory
static void WorkerFile(const ArcWork_t* w, int k, const char* what, char* out, size_t n)
{
	char name[64];
	snprintf(name, sizeof name, ".bgidump-%d.%s", k, what);
	DumpFs_Join(out, n, w->ctx.opt->outDir, name);
}

/* worker `k` of an archive: the entries whose index is k modulo the number
 * of workers, their lines ("E <index> <object>") and a closing line of
 * counts ("S <problems> <entries of each kind>") into its part file.
 * Returns 1 when the dump has to stop. */
static int Worker(int k, void* param)
{
	const ArcWork_t* w = (const ArcWork_t*)param;
	DumpCtx_t c = w->ctx;
	char path[DUMP_PATH_MAX], scratchPath[DUMP_PATH_MAX], sinkPath[DUMP_PATH_MAX];
	uint32_t kinds[DUMP_KIND_COUNT], i;
	FILE *part, *f;
	int j;
	memset(kinds, 0, sizeof kinds);
	WorkerFile(w, k, "part", path, sizeof path);
	WorkerFile(w, k, "tmp", scratchPath, sizeof scratchPath);
	WorkerFile(w, k, "log", sinkPath, sizeof sinkPath);
	part = OS_NativeOpen(path, "wb");
	c.scratch = OS_NativeOpen(scratchPath, "w+b");
	c.sink = OS_NativeOpen(sinkPath, "w+b");
	f = OS_NativeOpen(w->arcPath, "rb");
	c.arc = f ? ArcRead_OpenFile(f, c.arcName) : NULL;
	if(!part || !c.scratch || !c.sink || !c.arc)
	{
		fprintf(c.log, "%s: cannot open the archive or the worker's files in the output directory\n", c.arcName);
		c.failed = 1;
	}
	for(i = 0; i < w->index->count && !c.failed; i++)
	{
		DumpText_t line;
		if((int)(i % (uint32_t)w->workers) != k)
			continue;
		memset(&line, 0, sizeof line);
		fseek(c.sink, 0, SEEK_SET); // the compilers' messages of one entry overwrite those of the last
		kinds[DumpEntry(&c, &c.arc->entries[i], w->bases[i], &line)]++;
		fprintf(part, "E %u %s\n", (unsigned)i, line.p);
		DumpText_Free(&line);
	}
	if(part)
	{
		fprintf(part, "S %u", (unsigned)c.problems);
		for(j = 0; j < DUMP_KIND_COUNT; j++)
			fprintf(part, " %u", (unsigned)kinds[j]);
		fprintf(part, "\n");
		if(fclose(part) != 0)
			c.failed = 1;
	}
	if(c.scratch)
		fclose(c.scratch);
	if(c.sink)
		fclose(c.sink);
	OS_NativeRemove(scratchPath);
	OS_NativeRemove(sinkPath);
	Dump_FreeFuncCache(&c);
	if(c.arc)
		ArcRead_Close(c.arc);
	return c.failed;
}

typedef struct DumpTotals
{
	uint32_t kinds[DUMP_KIND_COUNT];
	uint32_t problems;
	uint32_t archives, entries;
} DumpTotals_t;

/* Dump the archive `fileName` of the game directory into the directory
 * `dirName` of the dump and append its object to the description.  Returns
 * 1 on success, 0 when the dump has to stop, -1 when the file is not an
 * archive (nothing is written for it). */
static int DumpArchive(const DumpCtx_t* base, const char* fileName, const char* dirName, DumpText_t* meta, DumpTotals_t* totals)
{
	const DumpOptions_t* opt = base->opt;
	ArcWork_t w;
	char arcPath[DUMP_PATH_MAX], utf[DUMP_NAME_MAX], path[DUMP_PATH_MAX];
	OsNativeStat_t st;
	FILE* f;
	ArcRead_t* a;
	char** lines;
	uint32_t kinds[DUMP_KIND_COUNT], problems = 0, i;
	int k, failed, first = 1;

	OS_NativeToUtf8(fileName, utf, sizeof utf);
	DumpFs_Join(arcPath, sizeof arcPath, opt->gameDir, utf);
	if(OS_NativeStat(arcPath, &st) != OS_NATIVE_FILE)
		return -1;
	{ // an archive begins with one of the two magics; anything else under the name is left alone, quietly
		uint8_t magic[12];
		f = OS_NativeOpen(arcPath, "rb");
		if(!f)
			return -1;
		if(fread(magic, 1, 12, f) != 12 || (memcmp(magic, "PackFile    ", 12) != 0 && memcmp(magic, "BURIKO ARC20", 12) != 0))
		{
			fclose(f);
			return -1;
		}
		fseek(f, 0, SEEK_SET);
	}
	a = ArcRead_OpenFile(f, utf);
	if(!a)
		return -1;

	memset(&w, 0, sizeof w);
	w.ctx = *base;
	w.ctx.arcName = utf;
	w.arcPath = arcPath;
	w.index = a;
	w.bases = MakeBases(a);
	DumpFs_Join(w.ctx.arcDir, sizeof w.ctx.arcDir, opt->outDir, dirName);
	// workers are worth their start only for an archive of some size
	w.workers = base->opt->jobs;
	if(w.workers > (int)a->count / 8)
		w.workers = (int)a->count / 8;
	if(w.workers < 1 || st.size < 0x200000)
		w.workers = 1;
	if(!OS_NativeMkdir(w.ctx.arcDir))
	{
		fprintf(base->log, "%s: cannot create the directory %s\n", utf, dirName);
		failed = 1;
	}
	else
		failed = OS_RunWorkers(w.workers, Worker, &w) != 0;

	// the workers' lines, back in entry order
	lines = (char**)BGI_Calloc(sizeof(char*) * ((size_t)a->count + 1));
	memset(kinds, 0, sizeof kinds);
	for(k = 0; k < w.workers; k++)
	{
		uint32_t size = 0;
		uint8_t* text;
		char* p;
		WorkerFile(&w, k, "part", path, sizeof path);
		text = DumpFs_Read(path, &size);
		OS_NativeRemove(path);
		if(!text)
		{
			failed = 1;
			continue;
		}
		for(p = (char*)text; *p;)
		{
			char* end = strchr(p, '\n');
			if(!end)
				break; // a line cut short: its worker did not finish
			*end = 0;
			if(p[0] == 'E')
			{
				char* rest;
				unsigned long index = strtoul(p + 2, &rest, 10);
				if(index < a->count && *rest == ' ' && !lines[index])
					lines[index] = BGI_Strdup(rest + 1);
			}
			else if(p[0] == 'S')
			{
				char* rest = p + 1;
				int j;
				problems += (uint32_t)strtoul(rest, &rest, 10);
				for(j = 0; j < DUMP_KIND_COUNT; j++)
					kinds[j] += (uint32_t)strtoul(rest, &rest, 10);
			}
			p = end + 1;
		}
		free(text);
	}
	for(i = 0; i < a->count; i++)
		if(!lines[i])
			failed = 1;

	if(!failed)
	{
		DumpText_Add(meta, "    {\"name\": ");
		DumpText_AddString(meta, utf);
		DumpText_Add(meta, ", \"dir\": ");
		DumpText_AddString(meta, dirName);
		DumpText_Printf(meta, ", \"type\": \"%s\", \"size\": %llu, \"entries\": [", a->arc20 ? "BURIKO ARC20" : "PackFile", (unsigned long long)st.size);
		for(i = 0; i < a->count; i++)
		{
			DumpText_Add(meta, i ? ",\n      " : "\n      ");
			DumpText_Add(meta, lines[i]);
		}
		DumpText_Add(meta, a->count ? "\n    ]}" : "]}");
		totals->archives++;
		totals->entries += a->count;
		totals->problems += problems;
		if(opt->verbose >= 1)
			fprintf(base->log, "%s: %u entries ->", utf, (unsigned)a->count);
		for(k = 0; k < DUMP_KIND_COUNT; k++)
		{
			totals->kinds[k] += kinds[k];
			if(kinds[k] && opt->verbose >= 1)
			{
				fprintf(base->log, "%s %u %s", first ? "" : ",", (unsigned)kinds[k], kDumpKindNames[k]);
				first = 0;
			}
		}
		if(opt->verbose >= 1)
		{
			fprintf(base->log, "\n");
			fflush(base->log);
		}
	}
	else
		fprintf(base->log, "%s: the archive could not be dumped\n", utf);
	for(i = 0; i < a->count; i++)
	{
		BGI_Free(lines[i]);
		BGI_Free(w.bases[i]);
	}
	BGI_Free(lines);
	BGI_Free(w.bases);
	ArcRead_Close(a);
	return failed ? 0 : 1;
}

// ---- the game ---------------------------------------------------------------------------------------

typedef struct GameInfo
{
	char match[256];   // the games.json entry's match string, "" when the game is not in the table
	char product[256]; // its product name
	char profile[ENGINE_PROFILE_NAME_MAX];
	EngineGen_t gen; // GEN_COUNT when no profile is known
} GameInfo_t;

// the boot program of the game directory: a loose ipl._bp, else system.arc's (BGI_Free it); NULL when there is none
static uint8_t* LoadBootProgram(const DumpOptions_t* opt, const NameList_t* files, uint32_t* size)
{
	const char* loose = FindName(files, "ipl._bp");
	const char* system = FindName(files, "system.arc");
	char path[DUMP_PATH_MAX], utf[DUMP_NAME_MAX];
	*size = 0;
	if(loose)
	{
		uint8_t* raw;
		OS_NativeToUtf8(loose, utf, sizeof utf);
		DumpFs_Join(path, sizeof path, opt->gameDir, utf);
		raw = DumpFs_Read(path, size);
		if(raw && *size >= 0x220 && BGI_IsDsc(raw))
		{
			uint8_t* out = (uint8_t*)BGI_Calloc((size_t)BGI_DscOutSize(raw) + 16);
			*size = BGI_DscDecode(raw, out);
			free(raw);
			return out;
		}
		if(raw)
			return raw;
	}
	if(system)
	{
		FILE* f;
		ArcRead_t* a;
		uint8_t* data = NULL;
		OS_NativeToUtf8(system, utf, sizeof utf);
		DumpFs_Join(path, sizeof path, opt->gameDir, utf);
		f = OS_NativeOpen(path, "rb");
		a = f ? ArcRead_OpenFile(f, utf) : NULL;
		if(a)
		{
			const ArcEntry_t* e = ArcRead_Find(a, "ipl._bp");
			int dsc;
			if(e)
				data = ArcRead_Load(a, e, size, &dsc);
			ArcRead_Close(a);
		}
		return data;
	}
	return NULL;
}

/* Identify the game: games.json (the one named, else the game directory's,
 * else the one beside the binary, else the built-in table) against the
 * boot program; --engine replaces the profile.  Returns 0 when the named
 * profile does not exist. */
static int DetectGame(const DumpOptions_t* opt, const NameList_t* files, FILE* log, GameInfo_t* g)
{
	char path[DUMP_PATH_MAX];
	uint32_t size = 0;
	uint8_t* ipl;
	int haveIpl;
	memset(g, 0, sizeof *g);
	g->gen = GEN_COUNT;
	Engine_ResetGames();
	if(opt->gamesJson)
	{
		if(!Engine_LoadGamesJson(opt->gamesJson))
			fprintf(log, "bgidump: cannot read %s: the built-in profiles and games are used\n", opt->gamesJson);
	}
	else
	{
		DumpFs_Join(path, sizeof path, opt->gameDir, "games.json");
		if(!Engine_LoadGamesJson(path))
		{
			OS_BinaryDir(path, sizeof path);
			snprintf(path + strlen(path), sizeof path - strlen(path), "games.json");
			if(!Engine_LoadGamesJson(path))
				Engine_ResetGames();
		}
	}
	ipl = LoadBootProgram(opt, files, &size);
	haveIpl = ipl != NULL;
	if(ipl)
	{
		int index = Engine_FindGame(ipl, size);
		EngineGame_t game;
		if(index >= 0 && Engine_GameAt(index, &game))
		{
			snprintf(g->match, sizeof g->match, "%s", game.match ? game.match : "");
			snprintf(g->product, sizeof g->product, "%s", game.product ? game.product : "");
			if(Engine_DetectGame(ipl, size))
			{
				snprintf(g->profile, sizeof g->profile, "%s", gEngine->name);
				g->gen = gEngine->gen;
			}
		}
		BGI_Free(ipl);
	}
	if(opt->engine)
	{
		if(strcmp(opt->engine, "any") == 0)
		{
			g->profile[0] = 0;
			g->gen = GEN_COUNT;
		}
		else if(!Engine_SelectProfile(opt->engine))
		{
			fprintf(log, "bgidump: unknown engine profile \"%s\" (bgi --list-engines prints them)\n", opt->engine);
			return 0;
		}
		else
		{
			snprintf(g->profile, sizeof g->profile, "%s", gEngine->name);
			g->gen = gEngine->gen;
		}
	}
	if(g->profile[0])
		fprintf(log, "bgidump: %s%sengine profile %s\n", g->product, g->product[0] ? ", " : "", g->profile);
	else
		fprintf(log, "bgidump: %s: the programs are read with the opcodes of every build (--engine=NAME names a profile)\n",
			haveIpl ? "the game is not in games.json" : "no boot program (ipl._bp) here to identify the game by");
	return 1;
}

// the game's scenario commands from its sysprg.arc into `cmds`; 1 when the table is there
static int LoadCommands(const DumpOptions_t* opt, const NameList_t* files, ScnCmdTable_t* cmds)
{
	const char* sysprg = FindName(files, "sysprg.arc");
	char path[DUMP_PATH_MAX], utf[DUMP_NAME_MAX];
	FILE* f;
	ArcRead_t* a;
	int ok;
	memset(cmds, 0, sizeof *cmds);
	if(!sysprg)
		return 0;
	OS_NativeToUtf8(sysprg, utf, sizeof utf);
	DumpFs_Join(path, sizeof path, opt->gameDir, utf);
	f = OS_NativeOpen(path, "rb");
	a = f ? ArcRead_OpenFile(f, utf) : NULL;
	if(!a)
		return 0;
	ok = Scn_ScanCommandsArc(a, utf, cmds);
	ArcRead_Close(a);
	return ok;
}

// the "scenario" member of the description: the command table, for the pack tool to compile with
static void WriteCommands(DumpText_t* meta, const ScnCmdTable_t* cmds)
{
	int i;
	DumpText_Printf(meta, "  \"scenario\": {\"scenes16\": %s, \"dispatch\": %u, \"commands\": [", cmds->scenes16 ? "true" : "false", (unsigned)cmds->dispatch);
	for(i = 0; i < cmds->count; i++)
	{
		const ScnCmd_t* c = &cmds->cmds[i];
		char params[sizeof c->params * 3];
		OS_SjisToUtf8(c->params, params, sizeof params);
		DumpText_Printf(meta, "%s\n    {\"token\": %u, \"module\": ", i ? "," : "", (unsigned)c->token);
		DumpText_AddString(meta, c->module);
		DumpText_Add(meta, ", \"name\": ");
		DumpText_AddString(meta, c->name);
		DumpText_Printf(meta, ", \"args\": %d, \"result\": %d, \"handler\": %u, \"params\": ", c->args, c->result, (unsigned)c->handler);
		DumpText_AddString(meta, params);
		DumpText_Add(meta, "}");
	}
	DumpText_Add(meta, cmds->count ? "\n  ]},\n" : "]},\n");
}

// ---- the run ----------------------------------------------------------------------------------------

int Dump_Run(const DumpOptions_t* opt)
{
	DumpOptions_t options = *opt;
	DumpCtx_t base;
	NameList_t files, archives, dirs, skipped;
	GameInfo_t game;
	ScnCmdTable_t cmds;
	DumpTotals_t totals;
	DumpText_t meta;
	char path[DUMP_PATH_MAX], source[DUMP_PATH_MAX], utf[DUMP_PATH_MAX];
	FILE* log = opt->log ? opt->log : stdout;
	int i, haveCmds, failed = 0, first = 1, limit = OS_WorkerLimit();

	memset(&files, 0, sizeof files);
	memset(&archives, 0, sizeof archives);
	memset(&dirs, 0, sizeof dirs);
	memset(&skipped, 0, sizeof skipped);
	memset(&totals, 0, sizeof totals);
	memset(&meta, 0, sizeof meta);
	if(options.jobs <= 0)
		options.jobs = limit > 16 ? 16 : limit;
	else if(options.jobs > limit)
		options.jobs = limit;
	if(OS_NativeList(opt->gameDir, CollectFile, &files) < 0)
	{
		fprintf(log, "bgidump: cannot read the directory %s\n", opt->gameDir);
		return 2;
	}
	qsort(files.names, (size_t)files.count, sizeof(char*), CompareNames);
	for(i = 0; i < files.count; i++)
	{
		char name[DUMP_NAME_MAX];
		int wanted;
		OS_NativeToUtf8(files.names[i], name, sizeof name);
		if(strlen(name) <= 4 || !Dump_EndsWithNoCase(name, ".arc"))
			continue;
		wanted = Dump_Wanted(name, opt->only, opt->onlyCount);
		if(!wanted)
		{ // an archive may be named without its ".arc"
			name[strlen(name) - 4] = 0;
			wanted = Dump_Wanted(name, opt->only, opt->onlyCount);
		}
		if(wanted)
			NameList_Add(&archives, files.names[i]);
	}
	if(archives.count == 0)
	{
		fprintf(log, "bgidump: no archives (*.arc) %sin %s\n", opt->onlyCount ? "of the names given " : "", opt->gameDir);
		NameList_Free(&files);
		return 2;
	}
	DumpFs_Join(path, sizeof path, opt->outDir, DUMP_META_NAME);
	if(OS_NativeStat(path, NULL) != OS_NATIVE_NONE && !opt->force)
	{ // a dump is somebody's working copy: nothing in it is overwritten by accident
		fprintf(log, "bgidump: %s holds a dump already (%s); --force writes over it, edited files included\n", opt->outDir, DUMP_META_NAME);
		NameList_Free(&files);
		NameList_Free(&archives);
		return 2;
	}
	if(!OS_NativeMkdir(opt->outDir))
	{
		fprintf(log, "bgidump: cannot create the directory %s\n", opt->outDir);
		NameList_Free(&files);
		NameList_Free(&archives);
		return 2;
	}
	if(!DetectGame(opt, &files, log, &game))
	{
		NameList_Free(&files);
		NameList_Free(&archives);
		return 2;
	}
	haveCmds = LoadCommands(opt, &files, &cmds);

	memset(&base, 0, sizeof base);
	base.opt = &options;
	base.log = log;
	base.gen = game.gen;
	base.cmds = haveCmds ? &cmds : NULL;
	base.pngLevel = opt->pngLevel > 0 ? (opt->pngLevel > 9 ? 9 : opt->pngLevel) : 4;
	base.entryName = "";
	fflush(log);

	OS_NativeAbsolute(opt->gameDir, source, sizeof source);
	OS_NativeToUtf8(source, utf, sizeof utf);
	DumpText_Printf(&meta, "{\n  \"format\": \"%s\",\n  \"version\": %d,\n  \"source\": ", DUMP_META_FORMAT, DUMP_META_VERSION);
	DumpText_AddString(&meta, utf);
	DumpText_Add(&meta, ",\n  \"game\": {\"match\": ");
	DumpText_AddString(&meta, game.match);
	DumpText_Add(&meta, ", \"product\": ");
	DumpText_AddString(&meta, game.product);
	DumpText_Add(&meta, ", \"profile\": ");
	DumpText_AddString(&meta, game.profile);
	DumpText_Add(&meta, ", \"generation\": ");
	DumpText_AddString(&meta, game.gen == GEN_COUNT ? "any" : kGenNames[game.gen]);
	DumpText_Add(&meta, "},\n");
	if(haveCmds)
		WriteCommands(&meta, &cmds);
	DumpText_Add(&meta, "  \"archives\": [\n");

	for(i = 0; i < archives.count && !failed; i++)
	{
		char name[DUMP_NAME_MAX], dir[DUMP_NAME_MAX + 16];
		size_t mark = meta.len;
		int n = 1, r;
		// the archive's directory: its name without ".arc", told apart from another archive's when need be
		OS_NativeToUtf8(archives.names[i], name, sizeof name);
		name[strlen(name) - 4] = 0;
		Dump_SafeName(name, dir, sizeof dir - 16);
		snprintf(name, sizeof name, "%s", dir);
		while(FindName(&dirs, dir))
			snprintf(dir, sizeof dir, "%s~%d", name, ++n);
		if(!first)
			DumpText_Add(&meta, ",\n");
		r = DumpArchive(&base, archives.names[i], dir, &meta, &totals);
		if(r > 0)
		{
			NameList_Add(&dirs, dir);
			first = 0;
		}
		else
		{
			meta.len = mark; // nothing of this archive stays in the description
			meta.p[mark] = 0;
			if(r < 0)
				NameList_Add(&skipped, archives.names[i]);
			else
				failed = 1;
		}
	}
	DumpText_Add(&meta, "\n  ],\n  \"skipped\": [");
	for(i = 0; i < skipped.count; i++)
	{
		char name[DUMP_NAME_MAX];
		OS_NativeToUtf8(skipped.names[i], name, sizeof name);
		DumpText_Add(&meta, i ? ", " : "");
		DumpText_AddString(&meta, name);
		fprintf(log, "%s: not an archive (no PackFile / BURIKO ARC20 header): left out\n", name);
	}
	DumpText_Add(&meta, "]\n}\n");

	if(!failed)
	{
		DumpFs_Join(path, sizeof path, opt->outDir, DUMP_META_NAME);
		if(!DumpFs_Write(path, meta.p, meta.len))
		{
			fprintf(log, "bgidump: cannot write %s\n", DUMP_META_NAME);
			failed = 1;
		}
	}
	if(!failed)
	{
		int k, any = 0;
		fprintf(log, "bgidump: %u archive%s, %u entries:", (unsigned)totals.archives, totals.archives == 1 ? "" : "s", (unsigned)totals.entries);
		for(k = 0; k < DUMP_KIND_COUNT; k++)
			if(totals.kinds[k])
			{
				fprintf(log, "%s %u %s", any ? "," : "", (unsigned)totals.kinds[k], kDumpKindNames[k]);
				any = 1;
			}
		fprintf(log, "\n");
		if(totals.problems)
			fprintf(log, "bgidump: %u entr%s with a note above\n", (unsigned)totals.problems, totals.problems == 1 ? "y" : "ies");
	}
	else
		fprintf(log, "bgidump: the dump is incomplete; no %s was written\n", DUMP_META_NAME);
	if(haveCmds)
		Scn_FreeCommands(&cmds);
	DumpText_Free(&meta);
	NameList_Free(&files);
	NameList_Free(&archives);
	NameList_Free(&dirs);
	NameList_Free(&skipped);
	return failed ? 2 : totals.problems ? 1
										: 0;
}
