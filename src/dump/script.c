/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * script.c - the programs and scenario files of the dump and pack tools
 *            (dump_internal.h): each is decompiled to the source it was
 *            written in and compiled from it again
 *
 * A program ("*._bp") goes through the decompiler of bpc.h and becomes
 * "<name>.bpc" (docs/bpc.md); a scenario file - the container of the
 * 1.494+ games, the headerless file of 1.66 / 1.69 or the 16-bit scene of
 * 1.64 - through that of scn.h and becomes "<name>.bsc"
 * (docs/scenario.md).  The scenario decompiler names the commands by the
 * table read from the game's sysprg.arc and takes the argument counts of
 * the functions a file calls from the files it imports, which are entries
 * of the same archive.
 *
 * Both decompilers check their output function by function; the dump
 * compiles the whole source once more and compares the result with the
 * entry, and records it ("roundtrip": false) when they differ: such a
 * file still packs to its original bytes while it is untouched, but an
 * edited one would differ from the original in more than the edit.
 */
#include "dump_internal.h"
#include "bgi/bpc.h"

// the text a decompiler wrote into the scratch file since its start (BGI_Free it)
static char* ScratchText(DumpCtx_t* c, size_t* len)
{
	long n = ftell(c->scratch);
	char* text;
	if(n < 0)
		n = 0;
	text = (char*)BGI_Alloc((size_t)n + 1);
	fseek(c->scratch, 0, SEEK_SET);
	if(n && fread(text, 1, (size_t)n, c->scratch) != (size_t)n)
		n = 0;
	text[n] = 0;
	*len = (size_t)n;
	return text;
}

// ---- programs ------------------------------------------------------------------------------------

/* a compiled program: a 16-byte header of the header size (0x10), the size
 * of the code area that follows and eight zero bytes */
static int IsProgram(const uint8_t* d, uint32_t n)
{
	uint32_t size;
	if(n < 16 || d[0] != 0x10 || d[1] || d[2] || d[3] || memcmp(d + 8, "\0\0\0\0\0\0\0\0", 8) != 0)
		return 0;
	memcpy(&size, d + 4, 4);
	return size + 16 == n;
}

int Dump_Program(DumpCtx_t* c, const uint8_t* data, uint32_t size)
{
	BpcDecOptions_t opt;
	BpcResult_t res;
	char rel[DUMP_NAME_MAX + 16];
	char* text;
	size_t len, stem;
	int blocks, same;
	if(!IsProgram(data, size))
		return 0;
	memset(&opt, 0, sizeof opt);
	opt.gen = c->gen;
	opt.name = c->entryName;
	fseek(c->scratch, 0, SEEK_SET);
	blocks = Bpc_Decompile(data, size, &opt, c->scratch);
	if(blocks < 0)
		return 0;
	text = ScratchText(c, &len);
	Bpc_Compile(text, len, c->entryName, c->gen, NULL, c->sink, &res);
	same = res.file && res.size == size && memcmp(res.file, data, size) == 0;
	BGI_Free(res.file);
	BGI_Free(res.labels);
	// "title._bp" becomes "title.bpc"
	stem = strlen(c->base);
	if(Dump_EndsWithNoCase(c->base, "._bp"))
		stem -= 4;
	snprintf(rel, sizeof rel, "%.*s.bpc", (int)stem, c->base);
	Dump_EmitFile(c, "file", rel, text, len);
	BGI_Free(text);
	if(blocks)
		DumpText_Printf(c->meta, ", \"blocks\": %d", blocks);
	if(!same)
	{
		DumpText_Add(c->meta, ", \"roundtrip\": false");
		Dump_Note(c, "the decompiled program does not compile back to the original bytes");
		c->problems++;
	}
	return 1;
}

uint8_t* Pack_Program(PackCtx_t* c, const JsonNode_t* entry, uint32_t* size)
{
	uint32_t len = 0;
	uint8_t* text = Pack_ReadMember(c, entry, "file", &len);
	const char* path = Json_String(Json_Get(entry, "file"), "path", c->entryName);
	BpcResult_t res;
	PrepIncludes_t inc;
	char full[DUMP_PATH_MAX], dir[DUMP_PATH_MAX];
	if(!text)
		return NULL;
	if(Json_Get(entry, "roundtrip") && Json_Number(entry, "roundtrip", 1) == 0)
		Pack_Note(c, "warning: this program did not compile back to its original bytes when it was dumped; the new one may differ in more than the edit");
	// an #include of the source is relative to where the source lies in the dump
	DumpFs_Join(full, sizeof full, c->arcDir, path);
	snprintf(dir, sizeof dir, "%.*s", (int)(OsCommon_FilePart(full) - full), full);
	memset(&inc, 0, sizeof inc);
	inc.baseDir = dir;
	Bpc_Compile((const char*)text, len, path, c->gen, &inc, c->log, &res);
	free(text);
	BGI_Free(res.labels);
	if(res.errors || !res.file)
	{
		BGI_Free(res.file);
		return NULL;
	}
	*size = res.size;
	return res.file;
}

// ---- scenario files ------------------------------------------------------------------------------

/* the functions of the scenario file `name` of the archive being dumped
 * (appended to list / count), read once per archive */
static void AddFunctionsOf(DumpCtx_t* c, const char* name, ScnFuncInfo_t** list, int* count)
{
	DumpFuncCache_t* fc = NULL;
	int i;
	for(i = 0; i < c->funcCacheCount && !fc; i++)
		if(Dump_EqualNoCase(c->funcCache[i].name, name))
			fc = &c->funcCache[i];
	if(!fc)
	{
		const ArcEntry_t* e = ArcRead_Find(c->arc, name);
		DumpFuncCache_t* grown = (DumpFuncCache_t*)BGI_Calloc(sizeof *grown * ((size_t)c->funcCacheCount + 1));
		if(c->funcCacheCount)
			memcpy(grown, c->funcCache, sizeof *grown * (size_t)c->funcCacheCount);
		BGI_Free(c->funcCache);
		c->funcCache = grown;
		fc = &c->funcCache[c->funcCacheCount++];
		snprintf(fc->name, sizeof fc->name, "%s", name);
		if(e)
		{
			uint32_t size;
			int dsc;
			uint8_t* data = ArcRead_Load(c->arc, e, &size, &dsc);
			ScnFile_t imp;
			char err[128];
			if(data && Scn_Load(data, size, &imp, err, sizeof err))
			{
				Scn_ListFunctions(&imp, &fc->funcs, &fc->count);
				Scn_Free(&imp);
			}
			BGI_Free(data);
		}
	}
	if(fc->count)
	{
		*list = (ScnFuncInfo_t*)realloc(*list, sizeof **list * ((size_t)*count + (size_t)fc->count));
		memcpy(*list + *count, fc->funcs, sizeof **list * (size_t)fc->count);
		*count += fc->count;
	}
}

void Dump_FreeFuncCache(DumpCtx_t* c)
{
	int i;
	for(i = 0; i < c->funcCacheCount; i++)
		free(c->funcCache[i].funcs);
	BGI_Free(c->funcCache);
	c->funcCache = NULL;
	c->funcCacheCount = 0;
}

int Dump_Scenario(DumpCtx_t* c, const uint8_t* data, uint32_t size)
{
	ScnFile_t f;
	ScnDecOptions_t opt;
	ScnCompileResult_t res;
	ScnFuncInfo_t* funcs = NULL;
	const ScnCmdTable_t* cmds = c->cmds && c->cmds->count ? c->cmds : NULL;
	char err[128], rel[DUMP_NAME_MAX + 16];
	char* text;
	size_t len;
	int funcCount = 0, blocks, same = 0, i;
	if(!Scn_Load(data, size, &f, err, sizeof err))
	{
		if(size >= 28 && memcmp(data, SCN_MAGIC, 27) == 0)
		{
			Dump_Note(c, "the scenario file does not load (%s): kept as it is", err);
			c->problems++;
		}
		return 0;
	}
	// a 16-bit scene is one only in a game whose system programs run such scenes
	if(f.image && !(c->cmds && c->cmds->scenes16))
	{
		Scn_Free(&f);
		return 0;
	}
	// a headerless scene calls the functions of its game's main file by address
	if(f.raw)
	{
		AddFunctionsOf(c, "main", &funcs, &funcCount);
		AddFunctionsOf(c, "mainscript", &funcs, &funcCount);
	}
	for(i = 0; i < f.importCount; i++)
		AddFunctionsOf(c, f.imports[i], &funcs, &funcCount);
	memset(&opt, 0, sizeof opt);
	opt.cmds = cmds;
	opt.funcs = funcs;
	opt.funcCount = funcCount;
	opt.name = c->entryName;
	fseek(c->scratch, 0, SEEK_SET);
	blocks = Scn_Decompile(&f, &opt, c->scratch);
	Scn_Free(&f);
	free(funcs);
	if(blocks < 0)
		return 0;
	text = ScratchText(c, &len);
	Scn_Compile(text, len, c->entryName, cmds, NULL, c->sink, &res);
	if(res.errors == 0)
	{
		uint32_t backSize = 0;
		uint8_t* back = Scn_Save(&res.file, &backSize);
		same = backSize == size && memcmp(back, data, size) == 0;
		BGI_Free(back);
	}
	Scn_Free(&res.file);
	snprintf(rel, sizeof rel, "%s.bsc", c->base);
	Dump_EmitFile(c, "file", rel, text, len);
	BGI_Free(text);
	if(blocks)
		DumpText_Printf(c->meta, ", \"blocks\": %d", blocks);
	if(!same)
	{
		DumpText_Add(c->meta, ", \"roundtrip\": false");
		Dump_Note(c, "the decompiled scenario does not compile back to the original bytes");
		c->problems++;
	}
	return 1;
}

uint8_t* Pack_Scenario(PackCtx_t* c, const JsonNode_t* entry, uint32_t* size)
{
	uint32_t len = 0;
	uint8_t* text = Pack_ReadMember(c, entry, "file", &len);
	const char* path = Json_String(Json_Get(entry, "file"), "path", c->entryName);
	ScnCompileResult_t res;
	PrepIncludes_t inc;
	char full[DUMP_PATH_MAX], dir[DUMP_PATH_MAX];
	uint8_t* out;
	if(!text)
		return NULL;
	if(Json_Get(entry, "roundtrip") && Json_Number(entry, "roundtrip", 1) == 0)
		Pack_Note(c, "warning: this scenario did not compile back to its original bytes when it was dumped; the new one may differ in more than the edit");
	// an #include of a 16-bit scene is relative to where the source lies in the dump
	DumpFs_Join(full, sizeof full, c->arcDir, path);
	snprintf(dir, sizeof dir, "%.*s", (int)(OsCommon_FilePart(full) - full), full);
	memset(&inc, 0, sizeof inc);
	inc.baseDir = dir;
	Scn_Compile((const char*)text, len, path, c->haveCmds ? &c->cmds : NULL, &inc, c->log, &res);
	free(text);
	if(res.errors)
	{
		Scn_Free(&res.file);
		return NULL;
	}
	out = Scn_Save(&res.file, size);
	Scn_Free(&res.file);
	return out;
}
