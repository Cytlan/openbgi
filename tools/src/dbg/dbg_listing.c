/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dbg_listing.c - the listings the debugger shows: a module's code area
 *                 through the disassembler, split into lines with offsets
 *                 (dbg_internal.h)
 *
 * A module's bytes are wrapped in a program header and handed to
 * Dis_Program with the offset column on; the listing is cached per
 * module (name, base, size) in a ring of MAX_LISTINGS entries, dropped
 * when the debugger closes.  A module reloaded at another base gets a
 * listing of its own.
 */
#include "dbg_internal.h"
#include "bgi/asm.h"
#include "bgi/vm.h"
#include "bgi/version.h"

#define MAX_LISTINGS 16 // listings cached at once; the oldest is replaced
static DbgListing_t gListings[MAX_LISTINGS];
static int gListingNext; // the slot the next listing takes

// free a slot's listing (nothing happens for an empty slot)
static void FreeListing(DbgListing_t* l)
{
	free(l->text);
	free(l->lines);
	free(l->offs);
	memset(l, 0, sizeof *l);
}

void DbgListing_FreeAll(void)
{
	int i;
	for(i = 0; i < MAX_LISTINGS; i++)
		FreeListing(&gListings[i]);
}

// The module of thread t whose code range holds `off`: its name, base and size; 1 found, 0 none (or no thread).
int Dbg_ModuleAt(Thread_t* t, uint32_t off, const char** name, uint32_t* base, uint32_t* size)
{
	ModuleNode_t* m;
	if(!t)
		return 0;
	for(m = t->modules; m; m = m->next)
	{
		if(off >= m->base && off < m->base + m->size)
		{
			*name = m->name;
			*base = m->base;
			*size = m->size;
			return 1;
		}
	}
	return 0;
}

/* The listing of `size` bytes at `base` of t's code area, cached under
 * "module:base:size".  A miss disassembles them: the bytes get the
 * 16-byte program header (the code offset 0x10 and the size, asm.h) and
 * go through Dis_Program into a temporary file, whose text is then split
 * into lines.
 * Each line's offset comes from the disassembler's offset column
 * ("XXXX  |"), which is stripped for the display; label and comment lines
 * take the offset of the instruction that follows them.  NULL when the
 * range lies outside the code area or no temporary file can be made. */
const DbgListing_t* DbgListing_Get(Thread_t* t, const char* module, uint32_t base, uint32_t size)
{
	char key[0x60];
	int i;
	DbgListing_t* l;
	FILE* tmp;
	uint8_t* file;
	DisOptions_t opt;
	long n;
	char* p;

	if(!t || base + size > t->codeSize || size == 0)
		return NULL;
	snprintf(key, sizeof key, "%s:%x:%x", module, (unsigned)base, (unsigned)size);
	for(i = 0; i < MAX_LISTINGS; i++)
		if(gListings[i].text && strcmp(gListings[i].key, key) == 0)
			return &gListings[i];
	l = &gListings[gListingNext];
	gListingNext = (gListingNext + 1) % MAX_LISTINGS;
	FreeListing(l);
	snprintf(l->key, sizeof l->key, "%s", key);

	// a program file from the module's bytes: the header, then the code
	file = (uint8_t*)malloc(16 + size);
	memset(file, 0, 16);
	file[0] = 0x10;
	file[4] = (uint8_t)size;
	file[5] = (uint8_t)(size >> 8);
	file[6] = (uint8_t)(size >> 16);
	file[7] = (uint8_t)(size >> 24);
	memcpy(file + 16, t->code + base, size);
	tmp = tmpfile();
	if(!tmp)
	{
		free(file);
		return NULL;
	}
	memset(&opt, 0, sizeof opt);
	opt.gen = gEngine->gen;
	opt.showOffsets = 1;
	opt.name = module;
	l->problems = Dis_Program(file, 16 + size, &opt, tmp);
	free(file);
	n = ftell(tmp);
	l->text = (char*)malloc((size_t)n + 1);
	fseek(tmp, 0, SEEK_SET);
	if(fread(l->text, 1, (size_t)n, tmp) != (size_t)n)
		n = 0;
	fclose(tmp);
	l->text[n] = 0;

	// split into lines; the offset column "XXXX  |" gives each line its offset, a label takes the next one
	l->count = 0;
	for(p = l->text; *p; p++)
		if(*p == '\n')
			l->count++;
	l->lines = (char**)malloc(((size_t)l->count + 1) * sizeof *l->lines);
	l->offs = (uint32_t*)malloc(((size_t)l->count + 1) * sizeof *l->offs);
	l->count = 0;
	p = l->text;
	while(*p)
	{
		char* nl = strchr(p, '\n');
		char* bar;
		if(nl)
			*nl = 0;
		l->lines[l->count] = p;
		l->offs[l->count] = ~0u;
		bar = strchr(p, '|');
		if(bar && bar - p >= 4 && (unsigned)(bar - p) < 12)
		{
			unsigned off;
			if(sscanf(p, "%x", &off) == 1)
			{
				l->offs[l->count] = off;
				// the instruction's text starts after the bar: drop the column for the display
				l->lines[l->count] = bar + 1;
				if(l->lines[l->count][0] == '\t')
					l->lines[l->count]++;
				// the label (and comment) lines before this one inherit the offset
				for(i = l->count - 1; i >= 0 && l->offs[i] == ~0u; i--)
					l->offs[i] = off;
			}
		}
		l->count++;
		if(!nl)
			break;
		p = nl + 1;
	}
	return l;
}

// The line showing offset `off`: the instruction line itself when there is one, else the first label or comment carrying it; -1 none.
int DbgListing_LineOf(const DbgListing_t* l, uint32_t off)
{
	int i, best = -1;
	if(!l)
		return -1;
	for(i = 0; i < l->count; i++)
	{
		size_t n;
		if(l->offs[i] != off)
			continue;
		n = strlen(l->lines[i]);
		if(n && l->lines[i][n - 1] != ':' && l->lines[i][0] != ';')
			return i; // the instruction line itself, not a label or comment before it
		if(best < 0)
			best = i;
	}
	return best;
}
