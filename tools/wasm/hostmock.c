/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * hostmock.c - see hostmock.h
 *
 * The lookup is server.js's: each component of a relative path is matched
 * against the directory's entries, exactly first, then without regard to
 * ASCII case; ".." is refused; "" is the root.  Reads are ranged and capped
 * at the server's 16 MB; nothing writes.
 */
#include "tools/wasm/hostmock.h"
#include "host/os.h"

#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>

static char gRoot[0x400] = ".";
static int gStatCalls, gReadCalls, gListCalls, gChanged;
static uint8_t* gListing;
static int gListingSize;

void HostMock_SetRoot(const char* dir)
{
	snprintf(gRoot, sizeof gRoot, "%s", dir);
}

void HostMock_Counts(int* stat, int* read, int* list, int* changed)
{
	*stat = gStatCalls;
	*read = gReadCalls;
	*list = gListCalls;
	*changed = gChanged;
}

static int Resolve(const char* rel, char* out, size_t n)
{
	const char* p = rel;
	snprintf(out, n, "%s", gRoot);
	while(*p)
	{
		char comp[0x200], found[0x200];
		size_t c = 0;
		DIR* d;
		struct dirent* e;
		while(*p == '/')
			p++;
		while(*p && *p != '/' && c + 1 < sizeof comp)
			comp[c++] = *p++;
		comp[c] = 0;
		if(!c || strcmp(comp, ".") == 0)
			continue;
		if(strcmp(comp, "..") == 0)
			return 0;
		d = opendir(out);
		if(!d)
			return 0;
		found[0] = 0;
		while((e = readdir(d)) != NULL)
		{
			if(strcmp(e->d_name, comp) == 0)
			{
				strcpy(found, e->d_name);
				break;
			}
			if(!found[0] && strcasecmp(e->d_name, comp) == 0)
				strcpy(found, e->d_name);
		}
		closedir(d);
		if(!found[0])
			return 0;
		snprintf(out + strlen(out), n - strlen(out), "/%s", found);
	}
	return 1;
}

static uint32_t AttrsOf(const struct stat* st, const char* name)
{
	uint32_t a = 0;
	if(S_ISDIR(st->st_mode))
		a |= OS_ATTR_DIRECTORY;
	if(!(st->st_mode & S_IWUSR))
		a |= OS_ATTR_READONLY;
	if(name[0] == '.')
		a |= OS_ATTR_HIDDEN;
	return a ? a : OS_ATTR_NORMAL;
}

int WasmHostJs_Connect(void)
{
	return 1;
}

void WasmHostJs_OverlayMount(const char* dir)
{
	mkdir(dir, 0755);
}

int WasmHostJs_Stat(const char* path, uint32_t* out)
{
	char p[0x400];
	struct stat st;
	uint64_t ft;
	const char* name;
	gStatCalls++;
	if(!Resolve(path, p, sizeof p) || stat(p, &st) != 0)
		return -1;
	name = strrchr(p, '/');
	out[0] = AttrsOf(&st, name ? name + 1 : p);
	out[1] = (uint32_t)st.st_size;
	out[2] = (uint32_t)((uint64_t)st.st_size >> 32);
	ft = ((uint64_t)st.st_mtime + 11644473600ull) * 10000000ull;
	out[3] = (uint32_t)ft;
	out[4] = (uint32_t)(ft >> 32);
	return 0;
}

int WasmHostJs_Read(const char* path, uint32_t offLo, uint32_t offHi, uint32_t len, void* dst)
{
	char p[0x400];
	FILE* f;
	size_t n;
	gReadCalls++;
	if(len > 16u * 1024u * 1024u || !Resolve(path, p, sizeof p))
		return -1;
	f = fopen(p, "rb");
	if(!f)
		return -1;
	fseek(f, (long)(((uint64_t)offHi << 32) | offLo), SEEK_SET);
	n = fread(dst, 1, len, f);
	fclose(f);
	return (int)n;
}

int WasmHostJs_ListSize(const char* path)
{
	char p[0x400];
	DIR* d;
	struct dirent* e;
	size_t cap = 0, used = 0;
	gListCalls++;
	free(gListing);
	gListing = NULL;
	gListingSize = 0;
	if(!Resolve(path, p, sizeof p) || (d = opendir(p)) == NULL)
		return -1;
	while((e = readdir(d)) != NULL)
	{
		char full[0x400];
		struct stat st;
		uint32_t attrs, lo, hi;
		uint16_t nlen = (uint16_t)strlen(e->d_name);
		if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		snprintf(full, sizeof full, "%s/%s", p, e->d_name);
		if(stat(full, &st) != 0)
			continue;
		attrs = AttrsOf(&st, e->d_name);
		lo = (uint32_t)st.st_size;
		hi = (uint32_t)((uint64_t)st.st_size >> 32);
		if(used + 14 + nlen > cap)
		{
			cap = (cap + 14 + nlen) * 2;
			gListing = (uint8_t*)realloc(gListing, cap);
		}
		memcpy(gListing + used, &attrs, 4);
		memcpy(gListing + used + 4, &lo, 4);
		memcpy(gListing + used + 8, &hi, 4);
		memcpy(gListing + used + 12, &nlen, 2);
		memcpy(gListing + used + 14, e->d_name, nlen);
		used += 14 + nlen;
	}
	closedir(d);
	gListingSize = (int)used;
	return gListingSize;
}

void WasmHostJs_ListTake(void* dst)
{
	if(gListing)
		memcpy(dst, gListing, (size_t)gListingSize);
}

void WasmHostJs_OverlayChanged(void)
{
	gChanged++;
}
