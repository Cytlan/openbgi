/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pack.c - Pack_Run: the archives of a game again, from a dump and its
 *          description (dump.h, docs/dump.md)
 *
 * For every archive of "bgidump.json" the entries are gone through twice.
 * First each one's files are compared with what the dump recorded - the
 * size, then the modification time, and the CRC-32 when the time differs -
 * which sorts the entries into those to copy and those to build.  Then
 * the archive is written: room for the index, the entries in order - an
 * unchanged one read from the original archive (and checked against the
 * recorded CRC, so that another archive of the same name is not taken for
 * it), a changed one made by its kind's converter and DSC-compressed when
 * the original was - and at last the index, which needs the sizes.
 *
 * The originals are only ever read.  An entry whose files do not build is
 * replaced by its original when there is one, with a message, so that one
 * mistake in one file does not hold up the whole archive; without an
 * original the archive is not written.
 */
#include "dump_internal.h"
#include "bgi/codec.h"

void Pack_Note(PackCtx_t* c, const char* fmt, ...)
{
	char msg[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	fprintf(c->log, "%s/%s: %s\n", c->arcName, c->entryName, msg);
}

uint8_t* Pack_ReadFileObject(PackCtx_t* c, const JsonNode_t* file, uint32_t* size)
{
	const char* rel = Json_String(file, "path", NULL);
	char path[DUMP_PATH_MAX];
	uint8_t* data;
	*size = 0;
	if(!rel || !Dump_IsInside(rel))
	{
		Pack_Note(c, rel ? "the description names a file outside the dump" : "the entry's description names no file");
		return NULL;
	}
	DumpFs_Join(path, sizeof path, c->arcDir, rel);
	data = DumpFs_Read(path, size);
	if(!data)
		Pack_Note(c, "cannot read %s", rel);
	return data;
}

uint8_t* Pack_ReadMember(PackCtx_t* c, const JsonNode_t* entry, const char* key, uint32_t* size)
{
	const JsonNode_t* file = Json_Get(entry, key);
	*size = 0;
	if(!file || file->type != JSON_OBJECT)
	{
		Pack_Note(c, "the entry's description has no \"%s\"", key);
		return NULL;
	}
	return Pack_ReadFileObject(c, file, size);
}

// ---- has an entry changed? -------------------------------------------------------------------------

#define FILE_SAME    0
#define FILE_CHANGED 1
#define FILE_MISSING 2

// one file of an entry against its record: size, then time, then the CRC
static int FileState(PackCtx_t* c, const JsonNode_t* file)
{
	const char* rel = Json_String(file, "path", NULL);
	const char* crcText = Json_String(file, "crc", NULL);
	char path[DUMP_PATH_MAX];
	OsNativeStat_t st;
	uint32_t crc;
	int ok;
	if(!rel || !Dump_IsInside(rel))
		return FILE_MISSING;
	DumpFs_Join(path, sizeof path, c->arcDir, rel);
	if(OS_NativeStat(path, &st) != OS_NATIVE_FILE)
		return FILE_MISSING;
	if(!crcText || !Json_Get(file, "size"))
		return FILE_CHANGED; // a file listed by hand: nothing to compare with
	if((double)st.size != Json_Number(file, "size", -1))
		return FILE_CHANGED;
	if(!c->opt->checksum && (double)st.mtime == Json_Number(file, "mtime", -1))
		return FILE_SAME;
	crc = DumpFs_Crc(path, &ok);
	return ok && crc == (uint32_t)strtoul(crcText, NULL, 16) ? FILE_SAME : FILE_CHANGED;
}

// the worst state among the files of an entry ("file", "info" and the elements of "frames")
static int EntryState(PackCtx_t* c, const JsonNode_t* entry)
{
	static const char* const members[] = {"file", "info"};
	const JsonNode_t* frames = Json_Get(entry, "frames");
	const JsonNode_t* f;
	int state = FILE_SAME, s;
	size_t i;
	for(i = 0; i < BGI_COUNTOF(members); i++)
	{
		f = Json_Get(entry, members[i]);
		if(f && (s = FileState(c, f)) > state)
			state = s;
	}
	for(f = frames && frames->type == JSON_ARRAY ? frames->child : NULL; f; f = f->next)
		if((s = FileState(c, f)) > state)
			state = s;
	return state;
}

// ---- building an entry -----------------------------------------------------------------------------

/* the entry's bytes as they are stored (BGI_Free them): its kind's
 * converter, then the DSC layer when the description says so.  NULL when
 * the files do not build (the converter said why). */
static uint8_t* BuildEntry(PackCtx_t* c, const JsonNode_t* entry, uint32_t* size)
{
	const char* kind = Json_String(entry, "kind", "data");
	uint8_t* data;
	if(strcmp(kind, "program") == 0)
		data = Pack_Program(c, entry, size);
	else if(strcmp(kind, "scenario") == 0)
		data = Pack_Scenario(c, entry, size);
	else if(strcmp(kind, "image") == 0)
		data = Pack_Image(c, entry, size);
	else if(strcmp(kind, "sound") == 0)
		data = Pack_Sound(c, entry, size);
	else if(strcmp(kind, "movie") == 0)
		data = Pack_Movie(c, entry, size);
	else if(strcmp(kind, "table") == 0)
		data = Pack_Table(c, entry, size);
	else if(strcmp(kind, "sdc") == 0)
		data = Pack_Sdc(c, entry, size);
	else if(strcmp(kind, "data") == 0)
		data = Pack_Data(c, entry, size);
	else
	{
		Pack_Note(c, "unknown kind \"%s\"", kind);
		return NULL;
	}
	if(data && Json_Number(entry, "dsc", 0) != 0)
	{
		uint32_t packed = 0;
		uint8_t* dsc = BGI_DscEncode(data, *size, BGI_Crc32(0, data, *size), &packed);
		uint8_t* check = (uint8_t*)BGI_Calloc((size_t)*size + 16);
		// the stream is decoded once here, where a fault costs a message, not in the game
		if(BGI_DscDecode(dsc, check) == *size && memcmp(check, data, *size) == 0)
		{
			BGI_Free(data);
			data = dsc;
			*size = packed;
		}
		else
		{
			Pack_Note(c, "warning: the DSC stream did not decode to its input: stored uncompressed");
			BGI_Free(dsc);
		}
		BGI_Free(check);
	}
	return data;
}

// the entry's name as the archive spells it (Shift-JIS) into `out`; 0 when it does not fit the format's name field
static int EntryName(const JsonNode_t* entry, char* out, size_t n, size_t field)
{
	const char* hex = Json_String(entry, "name_sjis", NULL);
	const char* name = Json_String(entry, "name", "");
	size_t len;
	if(hex)
	{
		int k = Dump_Unhex(hex, (uint8_t*)out, n - 1);
		if(k < 0)
			return 0;
		out[k] = 0;
	}
	else
		OS_Utf8ToSjis(name, out, n);
	len = strlen(out);
	return len > 0 && len <= field;
}

// ---- one archive ------------------------------------------------------------------------------------

#define PLAN_COPY  0 // from the original archive
#define PLAN_BUILD 1 // from the files

typedef struct PackTotals
{
	uint32_t archives, entries, rebuilt, fellBack;
	uint32_t unchanged; // archives left out because nothing of them changed
	int fatal;
} PackTotals_t;

static void PutLe32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

// read an entry's stored bytes from the original and check them against the recorded CRC (BGI_Free); NULL when they are not there
static uint8_t* ReadOriginal(FILE* src, const JsonNode_t* entry, uint32_t* size)
{
	const char* crcText = Json_String(entry, "crc", NULL);
	uint32_t n = (uint32_t)Json_Number(entry, "size", 0);
	uint64_t offset = (uint64_t)Json_Number(entry, "offset", 0);
	uint8_t* data;
	if(!src || !crcText || !Json_Get(entry, "offset") || !Json_Get(entry, "size"))
		return NULL;
	data = (uint8_t*)BGI_Alloc((size_t)n + 1);
	if(!OS_NativeSeek(src, offset) || fread(data, 1, n, src) != n || BGI_Crc32(0, data, n) != (uint32_t)strtoul(crcText, NULL, 16))
	{
		BGI_Free(data);
		return NULL;
	}
	*size = n;
	return data;
}

/* Build one archive of the description into the output directory.  Returns
 * 1 when it was written, 0 when it was left out (nothing changed and only
 * changed archives were asked for), -1 when it could not be written. */
static int PackArchive(PackCtx_t* c, const JsonNode_t* arc, const char* sourceDir, PackTotals_t* totals)
{
	const PackOptions_t* opt = c->opt;
	const char* name = Json_String(arc, "name", NULL);
	const char* dir = Json_String(arc, "dir", NULL);
	const JsonNode_t* entries = Json_Get(arc, "entries");
	const JsonNode_t* e;
	int arc20 = strcmp(Json_String(arc, "type", "PackFile"), "BURIKO ARC20") == 0;
	uint32_t recSize = arc20 ? 0x80 : 32, nameLen = arc20 ? 0x60 : 16;
	uint32_t count, i, changed = 0, rebuilt = 0, fellBack = 0, indexSize;
	uint64_t pos = 0;
	uint8_t* plan;
	uint8_t* index;
	char srcPath[DUMP_PATH_MAX], outPath[DUMP_PATH_MAX], srcFull[DUMP_PATH_MAX], outFull[DUMP_PATH_MAX];
	FILE *src = NULL, *out;
	OsNativeStat_t st;
	int ok = 1, missing = 0;

	if(!name || !dir || !entries || entries->type != JSON_ARRAY || !Dump_IsInside(name) || !Dump_IsInside(dir) || strchr(name, '/'))
	{
		fprintf(c->log, "bgipack: an archive of the description lacks its name, directory or entries, or names a place outside the dump\n");
		return -1;
	}
	c->arcName = name;
	c->entryName = "";
	DumpFs_Join(c->arcDir, sizeof c->arcDir, opt->dumpDir, dir);
	count = (uint32_t)Json_Count(entries);
	indexSize = 16 + count * recSize;

	// the original, when it is the archive that was dumped
	DumpFs_Join(srcPath, sizeof srcPath, sourceDir, name);
	DumpFs_Join(outPath, sizeof outPath, opt->outDir, name);
	OS_NativeAbsolute(srcPath, srcFull, sizeof srcFull);
	OS_NativeAbsolute(outPath, outFull, sizeof outFull);
	if(Dump_EqualNoCase(srcFull, outFull))
	{
		fprintf(c->log, "%s: the output would overwrite the original archive; choose another output directory\n", name);
		return -1;
	}
	if(!opt->rebuildAll && sourceDir[0] && OS_NativeStat(srcPath, &st) == OS_NATIVE_FILE)
	{
		if((double)st.size == Json_Number(arc, "size", -1))
			src = OS_NativeOpen(srcPath, "rb");
		else
			fprintf(c->log, "%s: the archive in the source directory is not the one that was dumped (its size differs): every entry is built from its files\n", name);
	}
	else if(!opt->rebuildAll)
		fprintf(c->log, "%s: no original archive in the source directory: every entry is built from its files\n", name);

	// which entries changed
	plan = (uint8_t*)BGI_Calloc((size_t)count + 1);
	for(e = entries->child, i = 0; e; e = e->next, i++)
	{
		int state;
		c->entryName = Json_String(e, "name", "?");
		state = EntryState(c, e);
		if(state == FILE_MISSING && !(src && Json_Get(e, "offset")))
		{
			Pack_Note(c, "a file of the entry is missing and there is no original to take it from");
			missing = 1;
		}
		if(state != FILE_SAME)
			changed++;
		plan[i] = (uint8_t)((state == FILE_SAME && src && Json_Get(e, "offset")) ? PLAN_COPY : PLAN_BUILD);
		if(state == FILE_MISSING && src && Json_Get(e, "offset"))
		{
			Pack_Note(c, "warning: a file of the entry is missing: the original entry is kept");
			plan[i] = PLAN_COPY;
			fellBack++;
		}
	}
	c->entryName = "";
	if(missing)
	{
		BGI_Free(plan);
		if(src)
			fclose(src);
		return -1;
	}
	if(opt->changedOnly && changed == 0)
	{
		totals->unchanged++;
		if(opt->verbose >= 2)
			fprintf(c->log, "%s: nothing changed, not written\n", name);
		BGI_Free(plan);
		if(src)
			fclose(src);
		return 0;
	}

	out = OS_NativeOpen(outPath, "wb");
	index = (uint8_t*)BGI_Calloc((size_t)indexSize + 1);
	if(!out || fwrite(index, 1, indexSize, out) != indexSize)
	{
		fprintf(c->log, "%s: cannot write the archive into the output directory\n", name);
		ok = 0;
	}
	memcpy(index, arc20 ? "BURIKO ARC20" : "PackFile    ", 12);
	PutLe32(index + 12, count);
	for(e = entries->child, i = 0; e && ok; e = e->next, i++)
	{
		uint8_t* rec = index + 16 + (size_t)i * recSize;
		uint8_t* data = NULL;
		uint32_t size = 0;
		char sjis[0x200];
		c->entryName = Json_String(e, "name", "?");
		if(!EntryName(e, sjis, sizeof sjis, nameLen))
		{
			Pack_Note(c, "the entry's name is empty or longer than the %u bytes an archive of this type holds", (unsigned)nameLen);
			ok = 0;
			break;
		}
		if(plan[i] == PLAN_COPY)
		{
			data = ReadOriginal(src, e, &size);
			if(!data)
			{ // not the bytes that were dumped after all: the files have to do
				Pack_Note(c, "the entry in the original archive is not the one that was dumped: built from its files");
				plan[i] = PLAN_BUILD;
			}
		}
		if(plan[i] == PLAN_BUILD)
		{
			data = BuildEntry(c, e, &size);
			if(data)
			{
				rebuilt++;
				if(opt->verbose >= 2)
					fprintf(c->log, "  %s: built from %s (%u bytes)\n", c->entryName, Json_String(Json_Get(e, "file"), "path", "its files"), (unsigned)size);
			}
			else
			{
				data = ReadOriginal(src, e, &size);
				if(data)
				{
					Pack_Note(c, "the entry does not build: THE ORIGINAL ENTRY IS KEPT in the new archive");
					fellBack++;
				}
				else
				{
					Pack_Note(c, "the entry does not build and there is no original to take it from");
					ok = 0;
					break;
				}
			}
		}
		if(pos + size > 0xffffffffu)
		{
			Pack_Note(c, "the archive would grow beyond 4 GB");
			ok = 0;
		}
		else if(size && fwrite(data, 1, size, out) != size)
		{
			fprintf(c->log, "%s: write error (is the disk full?)\n", name);
			ok = 0;
		}
		memcpy(rec, sjis, strlen(sjis));
		PutLe32(rec + nameLen, (uint32_t)pos); // counted from the end of the index
		PutLe32(rec + nameLen + 4, size);
		pos += size;
		BGI_Free(data);
	}
	c->entryName = "";
	if(ok && (!OS_NativeSeek(out, 0) || fwrite(index, 1, indexSize, out) != indexSize))
	{
		fprintf(c->log, "%s: cannot write the archive's index\n", name);
		ok = 0;
	}
	if(out && fclose(out) != 0)
		ok = 0;
	if(src)
		fclose(src);
	BGI_Free(index);
	BGI_Free(plan);
	if(!ok)
	{
		OS_NativeRemove(outPath); // half an archive is worse than none
		return -1;
	}
	totals->entries += count;
	totals->rebuilt += rebuilt;
	totals->fellBack += fellBack;
	if(opt->verbose >= 1)
	{
		fprintf(c->log, "%s: %u entries, %u built from files, %u copied", name, (unsigned)count, (unsigned)rebuilt, (unsigned)(count - rebuilt));
		if(fellBack)
			fprintf(c->log, " (%u of them because their files did not build)", (unsigned)fellBack);
		fprintf(c->log, "\n");
		fflush(c->log);
	}
	return 1;
}

// ---- the run ----------------------------------------------------------------------------------------

// the scenario commands the dump recorded, as the compiler's table
static void ReadCommands(const JsonNode_t* scenario, ScnCmdTable_t* cmds)
{
	const JsonNode_t* list = Json_Get(scenario, "commands");
	const JsonNode_t* n;
	int i = 0;
	memset(cmds, 0, sizeof *cmds);
	cmds->scenes16 = Json_Number(scenario, "scenes16", 0) != 0;
	cmds->dispatch = (uint32_t)Json_Number(scenario, "dispatch", 0);
	cmds->count = Json_Count(list);
	cmds->cmds = (ScnCmd_t*)calloc((size_t)cmds->count + 1, sizeof *cmds->cmds);
	for(n = list ? list->child : NULL; n; n = n->next, i++)
	{
		ScnCmd_t* c = &cmds->cmds[i];
		c->token = (uint32_t)Json_Number(n, "token", 0);
		snprintf(c->module, sizeof c->module, "%s", Json_String(n, "module", ""));
		snprintf(c->name, sizeof c->name, "%s", Json_String(n, "name", ""));
		c->args = (int)Json_Number(n, "args", -1);
		c->result = (int)Json_Number(n, "result", 0);
		c->handler = (uint32_t)Json_Number(n, "handler", 0);
		OS_Utf8ToSjis(Json_String(n, "params", ""), c->params, sizeof c->params);
	}
}

int Pack_Run(const PackOptions_t* opt)
{
	PackCtx_t c;
	PackTotals_t totals;
	char path[DUMP_PATH_MAX], sourceDir[DUMP_PATH_MAX];
	uint32_t size = 0;
	uint8_t* text;
	JsonNode_t* root;
	const JsonNode_t *game, *archives, *a;
	const char* profile;
	size_t err = 0;
	FILE* log = opt->log ? opt->log : stdout;
	int found = 0, g;

	memset(&c, 0, sizeof c);
	memset(&totals, 0, sizeof totals);
	c.opt = opt;
	c.log = log;
	c.arcName = "";
	c.entryName = "";
	DumpFs_Join(path, sizeof path, opt->dumpDir, DUMP_META_NAME);
	text = DumpFs_Read(path, &size);
	if(!text)
	{
		fprintf(log, "bgipack: %s has no %s: not a dump made by bgidump\n", opt->dumpDir, DUMP_META_NAME);
		return 2;
	}
	root = Json_Parse((const char*)text, size, &err);
	free(text);
	if(!root || strcmp(Json_String(root, "format", ""), DUMP_META_FORMAT) != 0)
	{
		if(!root)
			fprintf(log, "bgipack: %s is not valid JSON (near byte %lu)\n", DUMP_META_NAME, (unsigned long)err);
		else
			fprintf(log, "bgipack: %s is not a dump description (\"format\" is not \"%s\")\n", DUMP_META_NAME, DUMP_META_FORMAT);
		Json_Free(root);
		return 2;
	}
	if(Json_Number(root, "version", 0) > DUMP_META_VERSION)
	{
		fprintf(log, "bgipack: the dump was made by a newer bgidump (description version %d, this tool reads %d)\n", (int)Json_Number(root, "version", 0),
			DUMP_META_VERSION);
		Json_Free(root);
		return 2;
	}
	archives = Json_Get(root, "archives");
	if(!archives || archives->type != JSON_ARRAY)
	{
		fprintf(log, "bgipack: %s lists no archives\n", DUMP_META_NAME);
		Json_Free(root);
		return 2;
	}

	// the engine build the programs are compiled for, and the scenario commands
	game = Json_Get(root, "game");
	profile = Json_String(game, "profile", "");
	c.gen = GEN_COUNT;
	Engine_ResetGames();
	if(profile[0] && Engine_SelectProfile(profile))
		c.gen = gEngine->gen;
	else
		for(g = 0; g < GEN_COUNT; g++)
			if(strcmp(kGenNames[g], Json_String(game, "generation", "")) == 0)
				c.gen = (EngineGen_t)g;
	if(Json_Get(root, "scenario"))
	{
		ReadCommands(Json_Get(root, "scenario"), &c.cmds);
		c.haveCmds = 1;
	}

	if(opt->sourceDir)
		snprintf(sourceDir, sizeof sourceDir, "%s", opt->sourceDir);
	else
		OS_Utf8ToNative(Json_String(root, "source", ""), sourceDir, sizeof sourceDir);
	if(!OS_NativeMkdir(opt->outDir))
	{
		fprintf(log, "bgipack: cannot create the directory %s\n", opt->outDir);
		Json_Free(root);
		return 2;
	}
	for(a = archives->child; a; a = a->next)
	{
		const char* name = Json_String(a, "name", "");
		char stem[DUMP_NAME_MAX];
		int r;
		snprintf(stem, sizeof stem, "%s", name);
		if(Dump_EndsWithNoCase(stem, ".arc"))
			stem[strlen(stem) - 4] = 0;
		if(!Dump_Wanted(name, opt->only, opt->onlyCount) && !Dump_Wanted(stem, opt->only, opt->onlyCount))
			continue;
		found++;
		r = PackArchive(&c, a, sourceDir, &totals);
		if(r > 0)
			totals.archives++;
		else if(r < 0)
			totals.fatal = 1;
	}
	if(!found)
		fprintf(log, "bgipack: no archive %sin the description\n", opt->onlyCount ? "of the names given " : "");
	else
		fprintf(log, "bgipack: %u archive%s written, %u entries, %u built from files%s\n", (unsigned)totals.archives, totals.archives == 1 ? "" : "s",
			(unsigned)totals.entries, (unsigned)totals.rebuilt, totals.fatal ? "; AN ARCHIVE COULD NOT BE WRITTEN (see above)" : "");
	if(totals.unchanged)
		fprintf(log, "bgipack: %u archive%s unchanged and not written\n", (unsigned)totals.unchanged, totals.unchanged == 1 ? "" : "s");
	if(totals.fellBack)
		fprintf(log, "bgipack: %u entr%s kept from the original because the files did not build (see above)\n", (unsigned)totals.fellBack,
			totals.fellBack == 1 ? "y was" : "ies were");
	if(c.haveCmds)
		Scn_FreeCommands(&c.cmds);
	Json_Free(root);
	return totals.fatal || !found ? 2 : totals.fellBack ? 1
														: 0;
}
